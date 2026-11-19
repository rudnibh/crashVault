// src/store/wal.cc — Write-Ahead Log implementation
//
// CRASH SAFETY PROOF for a versioning operation:
//
//   Timeline:                             Crash here →     Recovery action
//   ─────────────────────────────────────────────────────────────────────────
//   1. wal.prepare(seq, chunks)           A                Discard: no manifest
//   2. obj_store.write_chunk(x) × N       B                Same as A
//   3. manifest_store.write_manifest(m)   C                Same as A (manifest gone)
//   4. wal.commit(seq)                    D                Re-write manifest if missing
//   5. manifest_store.append_history(m)   E                Re-append history (idempotent)
//   6. wal.complete(seq)                  —                Nothing to do
//
// At D, the WAL entry is PREPARE+COMMIT. On replay, we detect that the
// manifest file exists (write_manifest is atomic) but history may not.
// We re-call append_version(path, manifest_id), which is idempotent because
// history is serialized under a mutex and we check for duplicates.
//
// At B/C, the WAL entry is PREPARE-only. Chunks may exist in the object store,
// but since they are content-addressed, they are valid data — we just don't
// have a manifest referencing them yet. The orphaned chunks are reclaimed by
// GC. We discard the WAL entry.
#include "wal.h"
#include "../daemon/logger.h"
#include "../daemon/failpoint.h"
#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <sstream>
#include <fstream>
#include <iomanip>

using json = nlohmann::json;
using ::stat;

namespace cv {

// ── ISO-8601 timestamp ────────────────────────────────────────────────────────
static std::string iso8601_now() {
    time_t t = time(nullptr);
    struct tm tm_buf{};
    gmtime_r(&t, &tm_buf);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return buf;
}

// ── WalEntry serialization ───────────────────────────────────────────────────
static const char *state_str(WalEntry::State s) {
    switch (s) {
        case WalEntry::State::PREPARE:  return "PREPARE";
        case WalEntry::State::COMMIT:   return "COMMIT";
        case WalEntry::State::COMPLETE: return "COMPLETE";
    }
    return "UNKNOWN";
}

static WalEntry::State state_from_str(const std::string &s) {
    if (s == "COMMIT")   return WalEntry::State::COMMIT;
    if (s == "COMPLETE") return WalEntry::State::COMPLETE;
    return WalEntry::State::PREPARE;
}

std::string WalEntry::to_json() const {
    json j;
    j["seq"]         = seq;
    j["state"]       = state_str(state);
    j["manifest_id"] = manifest_id;
    j["file_path"]   = file_path;
    j["version"]     = version;
    j["chunk_shas"]  = chunk_shas;
    j["timestamp"]   = timestamp;
    return j.dump();  // single line — easier to verify atomicity
}

bool WalEntry::from_json(const std::string &json_str) {
    try {
        json j   = json::parse(json_str);
        seq         = j.at("seq").get<uint64_t>();
        state       = state_from_str(j.at("state").get<std::string>());
        manifest_id = j.at("manifest_id").get<std::string>();
        file_path   = j.at("file_path").get<std::string>();
        version     = j.at("version").get<uint32_t>();
        chunk_shas  = j.at("chunk_shas").get<std::vector<std::string>>();
        timestamp   = j.at("timestamp").get<std::string>();
        return true;
    } catch (...) {
        return false;
    }
}

// ── Wal implementation ───────────────────────────────────────────────────────
Wal::Wal(const std::string &store_root)
    : wal_dir_(store_root + "/wal")
{
    mkdir(wal_dir_.c_str(), 0755);

    // Find the highest existing sequence number for the counter
    uint64_t max_seq = 0;
    DIR *dir = opendir(wal_dir_.c_str());
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != nullptr) {
            std::string name = ent->d_name;
            if (name.size() < 4) continue;
            char *end = nullptr;
            uint64_t seq = strtoull(name.c_str(), &end, 10);
            if (end && (*end == '.' || *end == '\0'))
                max_seq = std::max(max_seq, seq);
        }
        closedir(dir);
    }
    seq_counter_.store(max_seq + 1);
}

std::string Wal::wal_path(uint64_t seq, bool done) const {
    // Zero-pad to 20 digits so lexicographic sort = chronological sort
    std::ostringstream ss;
    ss << wal_dir_ << "/" << std::setw(20) << std::setfill('0') << seq;
    ss << (done ? ".wal.done" : ".wal");
    return ss.str();
}

// Write a WAL entry using temp + fsync + rename (same crash-safe pattern as chunks)
bool Wal::write_entry(const WalEntry &e) const {
    std::string final_path = wal_path(e.seq, e.state == WalEntry::State::COMPLETE);
    std::string tmp_path   = final_path + ".tmp." + std::to_string(getpid());

    int fd = open(tmp_path.c_str(),
                  O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;

    std::string json_line = e.to_json() + "\n";
    ssize_t w = write(fd, json_line.data(), json_line.size());
    if (w < 0 || (size_t)w != json_line.size()) {
        close(fd); unlink(tmp_path.c_str()); return false;
    }

    // fsync the data
    if (fsync(fd) != 0) { close(fd); unlink(tmp_path.c_str()); return false; }
    close(fd);

    // Atomic rename into place
    if (rename(tmp_path.c_str(), final_path.c_str()) != 0) {
        unlink(tmp_path.c_str()); return false;
    }

    // fsync the WAL directory to make the rename durable
    // WHY: Without this, the rename may survive in the page cache but not on
    // disk. A subsequent crash could leave neither the old nor the new entry.
    int dir_fd = open(wal_dir_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd >= 0) { fsync(dir_fd); close(dir_fd); }

    return true;
}

bool Wal::read_entry(const std::string &path, WalEntry &out) const {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    if (!std::getline(f, line)) return false;
    return out.from_json(line);
}

uint64_t Wal::prepare(const std::string &manifest_id, const std::string &file_path,
                       uint32_t version,
                       const std::vector<std::string> &chunk_shas) {
    uint64_t seq = seq_counter_.fetch_add(1);

    WalEntry e;
    e.seq         = seq;
    e.state       = WalEntry::State::PREPARE;
    e.manifest_id = manifest_id;
    e.file_path   = file_path;
    e.version     = version;
    e.chunk_shas  = chunk_shas;
    e.timestamp   = iso8601_now();

    if (!write_entry(e)) {
        LOG_ERROR("WAL prepare failed for seq=" + std::to_string(seq));
        return 0;
    }
    CV_FAILPOINT("wal_after_prepare");
    return seq;
}

bool Wal::commit(uint64_t seq) {
    // Read the existing PREPARE entry
    WalEntry e;
    if (!read_entry(wal_path(seq, false), e)) {
        LOG_ERROR("WAL commit: cannot read PREPARE entry seq=" + std::to_string(seq));
        return false;
    }
    e.state = WalEntry::State::COMMIT;

    // Remove old entry, write new COMMIT entry
    // WHY overwrite the same file: We use O_TRUNC so the new COMMIT state
    // atomically replaces the PREPARE state via the same rename pattern.
    if (!write_entry(e)) {
        LOG_ERROR("WAL commit: write failed seq=" + std::to_string(seq));
        return false;
    }
    CV_FAILPOINT("wal_after_commit");
    return true;
}

bool Wal::complete(uint64_t seq) {
    WalEntry e;
    // Try reading COMMIT state first, fall back to PREPARE
    if (!read_entry(wal_path(seq, false), e)) {
        LOG_WARN("WAL complete: cannot find entry seq=" + std::to_string(seq));
        return false;
    }
    e.state = WalEntry::State::COMPLETE;

    // Write to .wal.done path
    if (!write_entry(e)) {
        LOG_ERROR("WAL complete: write failed seq=" + std::to_string(seq));
        return false;
    }

    // Remove the old .wal file (best-effort — we won't retry if this fails,
    // since the .wal.done file is the authoritative completed record)
    unlink(wal_path(seq, false).c_str());
    CV_FAILPOINT("wal_after_complete");
    return true;
}

std::vector<Wal::ReplayEntry> Wal::replay() const {
    std::vector<ReplayEntry> result;

    DIR *dir = opendir(wal_dir_.c_str());
    if (!dir) return result;

    std::vector<std::string> pending_paths;
    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        std::string name = ent->d_name;
        // Only look at .wal files (not .wal.done — those are complete)
        if (name.size() < 4) continue;
        if (name.rfind(".wal") == name.size() - 4 &&
            name.rfind(".wal.done") != name.size() - 9) {
            pending_paths.push_back(wal_dir_ + "/" + name);
        }
    }
    closedir(dir);
    std::sort(pending_paths.begin(), pending_paths.end());

    for (auto &path : pending_paths) {
        WalEntry e;
        if (!read_entry(path, e)) {
            LOG_WARN("WAL replay: cannot parse " + path + " — skipping");
            continue;
        }

        ReplayEntry re;
        re.entry = e;
        re.needs_history_update = (e.state == WalEntry::State::COMMIT);

        LOG_INFO("WAL replay: seq=" + std::to_string(e.seq) +
                 " state=" + state_str(e.state) +
                 " path=" + e.file_path);
        result.push_back(re);
    }

    return result;
}

void Wal::gc(size_t keep_last_n) {
    DIR *dir = opendir(wal_dir_.c_str());
    if (!dir) return;

    std::vector<std::string> done_paths;
    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        std::string name = ent->d_name;
        if (name.rfind(".wal.done") == name.size() - 9) {
            done_paths.push_back(wal_dir_ + "/" + name);
        }
    }
    closedir(dir);

    std::sort(done_paths.begin(), done_paths.end());

    // Keep the most recent keep_last_n; remove the rest
    size_t to_remove = done_paths.size() > keep_last_n
                       ? done_paths.size() - keep_last_n : 0;
    for (size_t i = 0; i < to_remove; i++) {
        unlink(done_paths[i].c_str());
    }
    if (to_remove > 0) {
        LOG_DEBUG("WAL GC: removed " + std::to_string(to_remove) + " completed entries");
    }
}

} // namespace cv
