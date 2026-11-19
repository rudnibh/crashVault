// src/store/versioner.h — File versioning engine (the worker's core logic)
//
// Called by each worker thread to process a settled file event.
// Performs:
//   1. Advisory flock(LOCK_SH|LOCK_NB) on the file
//   2. stat() before and after reading — detect concurrent modification
//   3. Chunk the file (FastCDC or fixed-size)
//   4. Store new chunks in the object store (dedup existing ones)
//   5. Write a Manifest (WAL-logged) recording the version
//   6. Append to per-path history
//
// WHY advisory lock:
//   flock(LOCK_SH|LOCK_NB) acquires a shared advisory lock. If a writer holds
//   LOCK_EX (exclusive), our LOCK_NB returns EWOULDBLOCK — we back off.
//   LIMITATION: Non-cooperating writers that don't call flock can still write
//   concurrently. We detect this via the before/after stat() comparison:
//   if mtime or size changed between lock acquire and read completion, we
//   treat the read as unstable and retry.
#pragma once
#include <string>
#include <memory>
#include <atomic>
#include <vector>
#include <cstdint>
#include <sys/stat.h>   // for struct stat
#include "chunker.h"
#include "object_store.h"
#include "manifest.h"
#include "../daemon/config.h"

namespace cv {

// Alias to avoid ambiguity between cv::stat (non-existent) and ::stat
using StatBuf = struct ::stat;


// Result of versioning one file
struct VersionResult {
    enum class Status {
        OK,              // Version stored successfully
        SKIPPED_DELETED, // File was deleted — no version stored
        SKIPPED_UNSTABLE,// File was being written to — skipped (safe)
        SKIPPED_IGNORED, // Pattern match or empty file
        ERROR            // I/O or store error
    };
    Status      status;
    std::string manifest_id; // set when status == OK
    size_t      chunks_written{0};
    size_t      chunks_deduped{0};
    size_t      bytes_stored{0};
};

class Versioner {
public:
    Versioner(const Config &cfg, ObjectStore &obj_store,
              ManifestStore &manifest_store);

    // Process one settled event. Thread-safe (each worker calls this independently).
    VersionResult process(const std::string &path, bool deleted);

    // Restore a file to a specific version (by manifest_id).
    // Writes to temp, fsyncs, then atomically renames into place.
    bool restore(const std::string &manifest_id,
                 const std::string &dest_path = "") const;

private:
    bool safe_read(const std::string &path, std::vector<uint8_t> &data,
                   StatBuf &st_out, VersionResult &result);

    const Config &cfg_;
    ObjectStore  &obj_store_;
    ManifestStore &manifest_store_;
    std::unique_ptr<IChunker> chunker_;
};

} // namespace cv
