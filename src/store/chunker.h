// src/store/chunker.h — Content-defined chunking (FastCDC) and fixed-size chunker
//
// WHY content-defined chunking (CDC):
// When a file is modified, fixed-size chunking shifts every boundary after
// the edit point, causing all subsequent chunks to be re-stored (poor dedup).
// CDC uses a rolling hash (gear hash) over a sliding window of bytes to find
// "natural" boundaries in the content. A small edit typically changes only
// 1–2 chunks, leaving all others identical and de-duplicated.
//
// FASTCDC ALGORITHM (Wen Xia et al., 2016):
//   - Gear hash: hash(h, byte) = (h << 1) + GEAR_TABLE[byte]
//   - Boundary condition: (hash & MASK_L) == 0  for small window (force boundary early)
//                         (hash & MASK_S) == 0  for normal window
//                         (hash & MASK_L) == 0  for large window (force boundary late)
//   - Min/avg/max chunk sizes prevent pathological cases
//
// Both chunkers implement the same ChunkerInterface so the pipeline can swap
// CDC vs fixed-size at runtime (required for the Phase 8 evaluation).
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <functional>

namespace cv {

// A single chunk: data + its SHA-256 hex digest
struct Chunk {
    std::vector<uint8_t> data;
    std::string          sha256;  // 64 hex chars
    size_t               offset;  // byte offset within the file
    size_t               size;    // == data.size()
};

// Callback: called once per chunk as it is produced
// WHY callback instead of collecting all chunks first: limits memory usage for
// large files — we can write each chunk to the object store as it is produced.
using ChunkCallback = std::function<void(Chunk &&)>;

// Abstract chunker interface — both FastCDC and fixed-size implement this
class IChunker {
public:
    virtual ~IChunker() = default;

    // Chunk the given data buffer, calling cb for each chunk
    virtual void chunk(const uint8_t *data, size_t size, ChunkCallback cb) = 0;

    // Convenience: chunk a file by path (uses mmap for large files)
    // Returns false if the file cannot be opened/read
    bool chunk_file(const std::string &path, size_t mmap_threshold, ChunkCallback cb);
};

// ── FastCDC chunker ──────────────────────────────────────────────────────────
struct FastCDCConfig {
    size_t min_size  = 2048;         // 2 KiB
    size_t avg_size  = 8192;         // 8 KiB
    size_t max_size  = 65536;        // 64 KiB
};

class FastCDCChunker : public IChunker {
public:
    explicit FastCDCChunker(const FastCDCConfig &cfg = {});
    void chunk(const uint8_t *data, size_t size, ChunkCallback cb) override;

private:
    // Find the next chunk boundary starting at offset, returning the chunk end
    size_t cut(const uint8_t *data, size_t pos, size_t remaining) const;

    FastCDCConfig cfg_;
    uint32_t      mask_s_;  // small window mask
    uint32_t      mask_l_;  // large window mask

    // Gear table: 256 pseudo-random 32-bit values used by the rolling hash
    static const uint32_t GEAR[256];
};

// ── Fixed-size chunker ───────────────────────────────────────────────────────
class FixedSizeChunker : public IChunker {
public:
    explicit FixedSizeChunker(size_t chunk_size = 8192);
    void chunk(const uint8_t *data, size_t size, ChunkCallback cb) override;

private:
    size_t chunk_size_;
};

} // namespace cv
