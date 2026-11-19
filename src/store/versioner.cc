// src/store/versioner.cc — File versioning engine implementation
#include "versioner.h"
#include "../../vendor/sha256.h"
#include "../daemon/logger.h"
#include "../daemon/failpoint.h"

// Use a using-declaration so "struct stat" unambiguously refers to the POSIX type,
// not anything in namespace cv.
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <utime.h>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <thread>
#include <chrono>
#include <sstream>

// Bring POSIX stat into scope so plain "struct stat" works inside namespace cv
using ::stat;

namespace cv {

Versioner::Versioner(const Config &cfg, ObjectStore &obj_store,
                     ManifestStore &manifest_store, Wal &wal)
    : cfg_(cfg)
    , obj_store_(obj_store)
    , manifest_store_(manifest_store)
    , wal_(wal)
    , buffer_pool_(cfg.memory_cap_bytes)
{
    FastCDCConfig cdc_cfg;
    chunker_ = std::make_unique<FastCDCChunker>(cdc_cfg);
}

// Safe file read: flock(LOCK_SH|LOCK_NB) + pre/post fstat to detect concurrent writes.
// WHY retry with exponential backoff: writers hold LOCK_EX briefly.
bool Versioner::safe_read(const std::string &path, std::vector<uint8_t> &data,
                           StatBuf &st_out, VersionResult &result,
                           BufferPool::Lease &read_lease) {
    constexpr int MAX_RETRIES = 5;
    int delay_ms = 50;

    for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
        int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            result.status = (errno == ENOENT)
                ? VersionResult::Status::SKIPPED_DELETED
                : VersionResult::Status::ERROR;
            return false;
        }

        // Try shared advisory lock (non-blocking).
        // WHY LOCK_SH|LOCK_NB: If a writer holds LOCK_EX we get EWOULDBLOCK and
        // back off. For filesystems without flock support we proceed unlocked
        // and rely on the before/after stat() comparison below.
        // NOTE: do not inspect errno after a successful flock() — it may hold a
        // stale value from an earlier call.
        if (flock(fd, LOCK_SH | LOCK_NB) != 0) {
            if (errno == EWOULDBLOCK) {
                close(fd);
                LOG_DEBUG("File locked by writer, retrying: " + path);
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                delay_ms *= 2;
                continue;
            }
            // flock unsupported — continue unlocked.
        }

        // stat BEFORE reading
        StatBuf st_before{};
        if (fstat(fd, &st_before) != 0) {
            flock(fd, LOCK_UN);
            close(fd);
            result.status = VersionResult::Status::ERROR;
            return false;
        }

        // Reserve memory before allocating. The lease is held by the caller
        // until hashing + chunking finish, so concurrent workers cannot exceed
        // the configured memory budget (except for one oversized file, which
        // runs alone; see BufferPool).
        size_t file_size = static_cast<size_t>(st_before.st_size);
        read_lease = buffer_pool_.acquire(file_size);

        // Read the whole file. read(2) may legally return short for large
        // files; loop until the expected byte count is reached.
        data.resize(file_size);
        size_t got = 0;
        bool read_error = false;
        while (got < data.size()) {
            ssize_t n = read(fd, data.data() + got, data.size() - got);
            if (n <= 0) { read_error = true; break; }
            got += static_cast<size_t>(n);
        }
        if (read_error) {
            read_lease.release();
            flock(fd, LOCK_UN);
            close(fd);
            result.status = VersionResult::Status::ERROR;
            return false;
        }

        // stat AFTER reading — detect concurrent modification
        // If mtime or size changed, the read may be inconsistent → retry.
        StatBuf st_after{};
        fstat(fd, &st_after);
        flock(fd, LOCK_UN);
        close(fd);

        if (st_before.st_mtime != st_after.st_mtime ||
            st_before.st_size  != st_after.st_size) {
            LOG_DEBUG("File changed during read (attempt " +
                      std::to_string(attempt + 1) + "): " + path);
            read_lease.release();
            data.clear();
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            delay_ms *= 2;
            continue;
        }

        st_out = st_before;
        return true;
    }

    LOG_WARN("Skipping unstable file (all retries exhausted): " + path);
    result.status = VersionResult::Status::SKIPPED_UNSTABLE;
    return false;
}

VersionResult Versioner::process(const std::string &path, bool deleted) {
    VersionResult result;
    result.status = VersionResult::Status::ERROR;

    if (deleted) {
        LOG_INFO("File deleted: " + path);
        result.status = VersionResult::Status::SKIPPED_DELETED;
        return result;
    }

    // ── 1. Safe read (bounded by the read budget) ────────────────────────────
    std::vector<uint8_t> data;
    StatBuf st{};
    BufferPool::Lease read_lease;
    if (!safe_read(path, data, st, result, read_lease)) return result;

    // ── 2. Whole-file SHA-256 ─────────────────────────────────────────────────
    std::string file_sha256 = sha256_hex(data.data(), data.size());

    // Idempotency: skip if content unchanged since last version
    auto history = manifest_store_.get_history(path);
    if (!history.empty()) {
        Manifest last;
        if (manifest_store_.read_manifest(history.back(), last) &&
            last.sha256 == file_sha256) {
            LOG_DEBUG("No change, skipping: " + path);
            result.status = VersionResult::Status::SKIPPED_IGNORED;
            return result;
        }
    }

    // ── 3. Chunk + store (chunks written before WAL prepare) ─────────────────
    Manifest m;
    m.id        = generate_uuid();
    m.path      = path;
    m.version   = static_cast<uint32_t>(history.size() + 1);
    m.file_size = data.size();
    m.mtime     = st.st_mtime;
    m.mode      = st.st_mode;
    m.sha256    = file_sha256;
    m.timestamp = time(nullptr);

    chunker_->chunk(data.data(), data.size(), [&](Chunk &&chunk) {
        m.chunks.push_back(chunk.sha256);
        if (obj_store_.write_chunk(chunk.sha256, chunk.data.data(), chunk.size)) {
            result.chunks_written++;
            result.bytes_stored += chunk.size;
        } else {
            result.chunks_deduped++;
        }
    });

    CV_FAILPOINT("versioner_after_chunks");

    // ── WAL PREPARE: intent recorded; crash here → orphaned chunks (safe, GC'd later)
    // If the WAL cannot be written we must not claim a crash-consistent
    // version: the chunks are harmless orphans and GC will reclaim them.
    uint64_t wal_seq = wal_.prepare(m.id, path, m.version, m.chunks);
    if (wal_seq == 0) {
        LOG_ERROR("WAL prepare failed; refusing to commit version for: " + path);
        result.status = VersionResult::Status::ERROR;
        return result;
    }

    // ── 4. Write manifest (atomic) ────────────────────────────────────────────
    if (!manifest_store_.write_manifest(m)) {
        LOG_ERROR("Failed to write manifest for: " + path);
        result.status = VersionResult::Status::ERROR;
        return result;
    }

    CV_FAILPOINT("versioner_after_manifest");

    // ── WAL COMMIT: manifest is durable; crash here → replay re-appends history
    if (!wal_.commit(wal_seq)) {
        LOG_WARN("WAL commit failed for seq=" + std::to_string(wal_seq) +
                 " — leaving entry for replay");
    }

    // ── 5. Update per-path history ────────────────────────────────────────────
    bool history_ok = manifest_store_.append_version(path, m.id);
    if (!history_ok) {
        LOG_WARN("Failed to update history for: " + path +
                 " — WAL entry left pending for replay");
    }

    CV_FAILPOINT("versioner_after_history");

    // ── WAL COMPLETE: all durable state written; entry can be GC'd ────────────
    // WHY only on success: a crash/replay must still see the entry if the
    // history update failed. Completing prematurely would lose that version
    // from the user-visible history even though its chunks/manifest exist.
    if (history_ok) wal_.complete(wal_seq);
    CV_FAILPOINT("versioner_after_complete");

    result.manifest_id = m.id;
    result.status = VersionResult::Status::OK;

    LOG_INFO("Versioned " + path + " v" + std::to_string(m.version) +
             " (" + std::to_string(m.chunks.size()) + " chunks, " +
             std::to_string(result.chunks_written) + " new, " +
             std::to_string(result.chunks_deduped) + " deduped)");
    return result;
}

bool Versioner::restore(const std::string &manifest_id,
                        const std::string &dest_path_override) const {
    Manifest m;
    if (!manifest_store_.read_manifest(manifest_id, m)) {
        LOG_ERROR("Cannot find manifest: " + manifest_id);
        return false;
    }

    std::string dest = dest_path_override.empty() ? m.path : dest_path_override;
    std::string tmp  = dest + ".cv_restore." + std::to_string(getpid());

    int fd = open(tmp.c_str(),
                  O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                  static_cast<mode_t>(m.mode & 0777u));
    if (fd < 0) {
        LOG_ERROR("Cannot open restore tmp " + tmp + ": " + strerror(errno));
        return false;
    }

    // Reassemble file from chunks
    for (auto &sha : m.chunks) {
        std::vector<uint8_t> chunk_data;
        if (!obj_store_.read_chunk(sha, chunk_data)) {
            LOG_ERROR("Missing chunk " + sha.substr(0, 16) +
                      "... during restore of " + m.path);
            close(fd);
            unlink(tmp.c_str());
            return false;
        }
        if (!chunk_data.empty()) {
            ssize_t w = write(fd, chunk_data.data(), chunk_data.size());
            if (w < 0 || static_cast<size_t>(w) != chunk_data.size()) {
                close(fd);
                unlink(tmp.c_str());
                return false;
            }
        }
    }

    // fsync + atomic rename
    if (fsync(fd) != 0) { close(fd); unlink(tmp.c_str()); return false; }
    close(fd);

    // Restore mtime via utimes (portable, no UTIME_OMIT needed)
    struct utimbuf ut;
    ut.actime  = m.mtime;
    ut.modtime = m.mtime;
    utime(tmp.c_str(), &ut);

    if (rename(tmp.c_str(), dest.c_str()) != 0) {
        LOG_ERROR("rename restore failed: " + std::string(strerror(errno)));
        unlink(tmp.c_str());
        return false;
    }

    LOG_INFO("Restored " + dest + " from manifest " + manifest_id);
    return true;
}

bool Versioner::restore_latest(const std::string &path,
                               const std::string &dest_path) const {
    auto history = manifest_store_.get_history(path);
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
        if (manifest_store_.has_manifest(*it))
            return restore(*it, dest_path);
    }
    LOG_ERROR("No restorable version for " + path);
    return false;
}

static void mkdir_p(const std::string &path) {
    if (path.empty()) return;
    for (size_t i = 1; i < path.size(); i++) {
        if (path[i] == '/') {
            std::string sub = path.substr(0, i);
            mkdir(sub.c_str(), 0755);
        }
    }
    mkdir(path.c_str(), 0755);
}

bool Versioner::restore_snapshot(const Snapshot &snap,
                                 const std::string &dest_root) const {
    bool all_ok = true;
    std::string root = snap.root;
    while (root.size() > 1 && root.back() == '/') root.pop_back();

    for (const auto &[path, manifest_id] : snap.files) {
        std::string dest = path;
        if (!dest_root.empty()) {
            std::string relative;
            if (!root.empty() && path.rfind(root, 0) == 0) {
                relative = path.substr(root.size());
                if (relative.empty() || relative[0] != '/') relative = "/" + relative;
            } else {
                // Not under the recorded root: keep only the basename so we
                // never write outside dest_root.
                size_t slash = path.find_last_of('/');
                relative = "/" + (slash == std::string::npos ? path : path.substr(slash + 1));
            }
            dest = dest_root + relative;
        }

        size_t slash = dest.find_last_of('/');
        if (slash != std::string::npos) mkdir_p(dest.substr(0, slash));

        if (!restore(manifest_id, dest)) {
            LOG_ERROR("Snapshot restore failed for " + path);
            all_ok = false;
        }
    }
    return all_ok;
}

} // namespace cv
