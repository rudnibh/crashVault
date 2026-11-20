// src/store/object_store.cc — Content-addressed chunk store implementation
#include "object_store.h"
#include "../../vendor/sha256.h"
#include "../daemon/logger.h"
#include "../daemon/failpoint.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <sstream>
#include <thread>
#include <vector>
#include <functional>

namespace cv {

static bool valid_sha256_id(const std::string &s) {
    if (s.size() != 64) return false;
    for (char c : s) {
        bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                   (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }
    return true;
}

ObjectStore::ObjectStore(const std::string &store_root)
    : store_root_(store_root)
    , objects_root_(store_root + "/objects")
{
    // Ensure objects/ root exists
    mkdir(objects_root_.c_str(), 0755);
}

std::string ObjectStore::chunk_path(const std::string &sha256) const {
    // Shard layout: objects/<2hex>/<62hex>
    // WHY: Having all chunks in one directory would create a directory with
    // potentially millions of entries, slowing readdir/stat significantly.
    // Sharding by first 2 hex chars gives 256 directories, capping entries
    // per directory at ~N/256 (e.g. 100K chunks → ~390 per shard).
    if (sha256.size() < 2) return {};
    return objects_root_ + "/" + sha256.substr(0, 2) + "/" + sha256.substr(2);
}

bool ObjectStore::ensure_shard_dir(const std::string &sha256) const {
    std::string dir = objects_root_ + "/" + sha256.substr(0, 2);
    if (mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
        LOG_ERROR("mkdir shard " + dir + ": " + strerror(errno));
        return false;
    }
    return true;
}

bool ObjectStore::has_chunk(const std::string &sha256) const {
    if (!valid_sha256_id(sha256)) return false;
    return access(chunk_path(sha256).c_str(), F_OK) == 0;
}

bool ObjectStore::write_chunk(const std::string &sha256,
                              const uint8_t *data, size_t size) {
    return put_chunk(sha256, data, size) == WriteResult::Written;
}

ObjectStore::WriteResult ObjectStore::put_chunk(const std::string &sha256,
                                                const uint8_t *data,
                                                size_t size) {
    if (!valid_sha256_id(sha256)) return WriteResult::Error;

    // Fast path: chunk already exists (dedup hit — no I/O needed)
    if (has_chunk(sha256)) return WriteResult::Existed;

    if (!ensure_shard_dir(sha256)) return WriteResult::Error;

    std::string final_path = chunk_path(sha256);
    // Temp file in the same directory (same filesystem = atomic rename possible).
    // mkstemp gives every writer (including concurrent workers and a restarted
    // process that reuses a PID) a unique name, so a stale temp file can never
    // be mistaken for an in-progress chunk.
    std::string tmp_template = final_path + ".tmp.XXXXXX";
    std::vector<char> tmp_buf(tmp_template.begin(), tmp_template.end());
    tmp_buf.push_back('\0');
    int fd = mkstemp(tmp_buf.data());
    if (fd < 0) {
        LOG_ERROR("mkstemp for chunk " + sha256 + ": " + strerror(errno));
        return WriteResult::Error;
    }
    std::string tmp_path(tmp_buf.data());
    int fd_flags = fcntl(fd, F_GETFD);
    if (fd_flags >= 0) fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);

    // ── Step 1: Write to temp file ────────────────────────────────────────────
    size_t written = 0;
    while (written < size) {
        ssize_t n = write(fd, data + written, size - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            LOG_ERROR("write chunk " + sha256 + ": " + strerror(errno));
            close(fd);
            unlink(tmp_path.c_str());
            return WriteResult::Error;
        }
        written += static_cast<size_t>(n);
    }

    // Chunks are immutable once published: make the file read-only. fchmod
    // happens before fsync so both data and mode are durable.
    if (fchmod(fd, 0444) != 0) {
        LOG_ERROR("chmod chunk " + sha256 + ": " + strerror(errno));
        close(fd);
        unlink(tmp_path.c_str());
        return WriteResult::Error;
    }

    CV_FAILPOINT("object_after_write");

    // ── Step 2: fsync — flush data to stable storage ──────────────────────────
    // WHY: Without fsync, the kernel may buffer the data. If the system crashes
    // before the buffer is flushed, the temp file contents are lost. We must
    // ensure the data is on disk before we rename.
    if (fsync(fd) != 0) {
        LOG_ERROR("fsync chunk " + sha256 + ": " + strerror(errno));
        close(fd);
        unlink(tmp_path.c_str());
        return WriteResult::Error;
    }
    close(fd);

    CV_FAILPOINT("object_after_fsync");

    // ── Step 3: Atomic rename into final position ─────────────────────────────
    // WHY rename is atomic: POSIX guarantees rename(2) is atomic on the same
    // filesystem — the directory entry atomically switches from old to new.
    // A reader either sees the old file or the new file, never partial content.
    if (rename(tmp_path.c_str(), final_path.c_str()) != 0) {
        LOG_ERROR("rename chunk " + sha256 + ": " + strerror(errno));
        unlink(tmp_path.c_str());
        return WriteResult::Error;
    }

    CV_FAILPOINT("object_after_rename");

    // ── Step 4: fsync the directory ───────────────────────────────────────────
    // WHY: The rename updated the directory entry in memory. Without fsync(dir),
    // a crash after rename but before the directory write-back could leave the
    // inode allocated but no directory entry pointing to it (lost data).
    std::string shard_dir = objects_root_ + "/" + sha256.substr(0, 2);
    int dir_fd = open(shard_dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd >= 0) {
        fsync(dir_fd); // ignore error — best effort; WAL will catch missing chunks
        close(dir_fd);
    }

    LOG_DEBUG("Stored chunk " + sha256.substr(0, 16) + "... (" +
              std::to_string(size) + " bytes)");
    return WriteResult::Written;
}

bool ObjectStore::read_chunk(const std::string &sha256,
                             std::vector<uint8_t> &out) {
    if (!valid_sha256_id(sha256)) return false;
    std::string path = chunk_path(sha256);
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;

    struct stat st{};
    if (fstat(fd, &st) != 0) { close(fd); return false; }

    out.resize(static_cast<size_t>(st.st_size));
    size_t got = 0;
    while (got < out.size()) {
        ssize_t n = read(fd, out.data() + got, out.size() - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); out.clear(); return false; }
        got += static_cast<size_t>(n);
    }
    close(fd);

    // Restore must never silently emit corrupted bytes. Re-hash on read so a
    // damaged object is treated exactly like a missing one.
    if (sha256_hex(out.data(), out.size()) != sha256) {
        out.clear();
        return false;
    }
    return true;
}

bool ObjectStore::verify_chunk(const std::string &sha256) const {
    if (!valid_sha256_id(sha256)) return false;
    std::string path = chunk_path(sha256);
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;

    struct stat st{};
    if (fstat(fd, &st) != 0) { close(fd); return false; }

    std::vector<uint8_t> data(static_cast<size_t>(st.st_size));
    size_t got = 0;
    while (got < data.size()) {
        ssize_t n = read(fd, data.data() + got, data.size() - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); return false; }
        got += static_cast<size_t>(n);
    }
    close(fd);

    std::string actual = sha256_hex(data.data(), data.size());
    return actual == sha256;
}

} // namespace cv
