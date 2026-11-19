// src/store/wal.h — Write-Ahead Log for crash-consistent version commits
//
// WHY a WAL:
// Without a WAL, a crash between "chunks written" and "manifest written" leaves
// orphaned chunks (wasted space) or, worse, a manifest referencing chunks that
// were never durably written.
//
// Our WAL records every versioning operation in three steps:
//   1. PREPARE:  all chunks for this operation are enumerated
//   2. COMMIT:   manifest_id written → operation is "committed" (durable)
//   3. COMPLETE: manifest & history files written → WAL entry can be GC'd
//
// On restart, we replay:
//   - PREPARE-only entries: chunks may be present but manifest is not → safe to
//     ignore (chunks are content-addressed; they won't corrupt other manifests)
//   - PREPARE+COMMIT entries: manifest exists but history may not → re-append
//     the manifest_id to history (idempotent)
//   - COMPLETE entries: nothing to do
//
// STRUCTURE:
//   wal/<seq>.wal  — one file per operation (atomic rename on COMPLETE)
//   Each .wal file is a JSON object written as a single fsync'd line.
//
// WHY per-operation files instead of a single append log:
//   - Single append log requires careful truncation/rotation on replay
//   - Per-file WAL: COMPLETE is simply rename(x.wal → x.wal.done); fsync(dir)
//   - Completed entries can be GC'd independently without log compaction
//   - Simpler to reason about crash safety
#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <atomic>

namespace cv {

struct WalEntry {
    enum class State {
        PREPARE,  // chunks written, manifest not yet committed
        COMMIT,   // manifest written (operation durable)
        COMPLETE  // history updated, WAL entry can be GC'd
    };

    uint64_t    seq;          // monotonically increasing sequence number
    State       state;
    std::string manifest_id;
    std::string file_path;
    uint32_t    version;
    std::vector<std::string> chunk_shas; // in order
    std::string timestamp;    // ISO-8601

    std::string to_json() const;
    bool        from_json(const std::string &json_str);
};

class Wal {
public:
    explicit Wal(const std::string &store_root);

    // Write a PREPARE entry. Returns the sequence number, or 0 on I/O failure.
    // WHY: We log the intent before writing any data, so a crash during write
    // is recoverable — we just discard the in-progress operation.
    uint64_t prepare(const std::string &manifest_id, const std::string &file_path,
                     uint32_t version,
                     const std::vector<std::string> &chunk_shas);

    // Advance entry to COMMIT state (call after manifest is durably written)
    bool commit(uint64_t seq);

    // Advance entry to COMPLETE state (call after history is updated)
    bool complete(uint64_t seq);

    // Replay on startup: return list of entries that need recovery
    // (entries in PREPARE or COMMIT state without a matching COMPLETE)
    struct ReplayEntry {
        WalEntry entry;
        bool needs_history_update; // true for COMMIT without COMPLETE
    };
    std::vector<ReplayEntry> replay() const;

    // Garbage-collect all COMPLETE entries older than keep_last_n
    void gc(size_t keep_last_n = 100);

    // Return the WAL directory
    const std::string &wal_dir() const { return wal_dir_; }

private:
    std::string wal_path(uint64_t seq, bool done) const;
    bool write_entry(const WalEntry &e) const;
    bool read_entry(const std::string &path, WalEntry &out) const;

    std::string   wal_dir_;
    std::atomic<uint64_t> seq_counter_{0};
};

} // namespace cv
