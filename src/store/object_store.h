// src/store/object_store.h — Content-addressed chunk store
//
// WHY content-addressable storage (CAS):
// Chunks are stored by their SHA-256 hash. Two files sharing identical
// 8-KiB blocks store that block only once. This is the deduplication engine.
//
// CRASH SAFETY:
// Every write follows the sequence:
//   1. Write chunk to a temp file in the same directory (same filesystem!)
//   2. fsync(temp_fd) — data is durable
//   3. rename(temp, objects/ab/cdef...) — atomic on POSIX (same filesystem)
//   4. fsync(dir_fd) — directory entry is durable
//
// WHY fsync the directory: without it, the rename may be durable but the
// directory entry update may still be in memory. A crash could leave the
// chunk file without a directory entry (orphaned inode, lost forever).
//
// Writing an existing chunk is a no-op (we check with access() first).
#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>

namespace cv {

class ObjectStore {
public:
    enum class WriteResult {
        Written,  // new chunk durably stored
        Existed,  // deduplication hit; chunk was already present
        Error     // I/O failure; the caller must not commit a manifest
    };

    // store_root: the .crashvault directory
    explicit ObjectStore(const std::string &store_root);

    // Store a chunk and report exactly what happened (including I/O errors).
    // Thread-safe: multiple workers may call this concurrently — unique temp
    // files plus atomic rename make concurrent writes of the same chunk safe.
    WriteResult put_chunk(const std::string &sha256,
                          const uint8_t *data, size_t size);

    // Backwards-compatible boolean convenience API for tests/tools:
    // true = newly written, false = existed or error. Production code uses
    // put_chunk() so it can distinguish a dedup hit from a real failure.
    bool write_chunk(const std::string &sha256,
                     const uint8_t *data, size_t size);

    // Read a chunk by its sha256. Returns false if not found or corrupt.
    bool read_chunk(const std::string &sha256,
                    std::vector<uint8_t> &out);

    // Check whether a chunk file exists. Use verify_chunk() to re-hash it.
    bool has_chunk(const std::string &sha256) const;

    // Verify a chunk: re-hash stored data and compare
    bool verify_chunk(const std::string &sha256) const;

    // Return the filesystem path for a given sha256
    std::string chunk_path(const std::string &sha256) const;

    // Return the objects/ root directory
    const std::string &objects_root() const { return objects_root_; }

private:
    // Ensure the shard directory exists (ab/ for first 2 hex chars)
    bool ensure_shard_dir(const std::string &sha256) const;

    std::string store_root_;
    std::string objects_root_;
};

} // namespace cv
