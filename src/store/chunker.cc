// src/store/chunker.cc — FastCDC and fixed-size chunker implementations
//
// FASTCDC PAPER: Wen Xia et al., "FastCDC: a Fast and Efficient Content-Defined
// Chunking Approach for Data Deduplication", USENIX ATC 2016.
//
// KEY INSIGHT: The gear hash is a Rabin-like rolling hash that is very fast
// (one multiply + one lookup per byte). Unlike Rabin-Karp, it doesn't need a
// sliding window subtraction — the hash naturally "forgets" old bytes as the
// window shifts because the shift-left discards the MSB.

#include "chunker.h"
#include "../../vendor/sha256.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <cassert>

namespace cv {

// ── Gear table: 256 pseudo-random values ─────────────────────────────────────
// WHY: The gear hash needs uniformly distributed values to get good boundary
// distribution. We use a standard set from the FastCDC reference implementation.
const uint32_t FastCDCChunker::GEAR[256] = {
    0xFCE9CC97, 0x0A405340, 0xA387FAF7, 0x3EBB5D4C, 0x848EB4D3, 0xC7721BDA,
    0xFAC441BF, 0xDF353671, 0xAE7CD451, 0xD1DD1A87, 0xA2ED0BF0, 0x9831F026,
    0xF054BDF6, 0x733039D7, 0xE490A9B9, 0x8A459D74, 0x0D445378, 0x4633A43C,
    0xDBCEB161, 0x1F035172, 0x3DF328CF, 0x3CA3A364, 0x857CE90B, 0xDF7340AB,
    0xB37AF792, 0xEEE6E022, 0x7AD59328, 0x6DCBADB3, 0xC6327994, 0x88F85343,
    0x7E62E664, 0xF23503A6, 0x1C1BCFAC, 0x078E8307, 0x5DF3310F, 0xD12903D8,
    0x635CDA3E, 0x1FAFB829, 0x5C16D218, 0x5A20809F, 0x7F7D791A, 0x826F79A5,
    0x1D6B2300, 0xA8D22AAF, 0x0E42DC8D, 0x6F6C25FF, 0xA84A9614, 0x9637C7CD,
    0xF489BDA3, 0x0DBC16F2, 0xC981578F, 0x3C562B85, 0x48773F50, 0x76ED8118,
    0xE5EFC9AC, 0x2B06F4ED, 0x26DF8732, 0x22596513, 0x44326AFC, 0x53ACC1EA,
    0xBB76B8CD, 0x4A0A7C43, 0xA55B2258, 0x31BB286C, 0x99668B05, 0xB80F12B8,
    0x0E0B47BE, 0xB9702392, 0x74CC5769, 0x61D84C72, 0x0F151B98, 0x327F7D0F,
    0xD18F0629, 0x14604416, 0xA857D8C4, 0xF8AE86EF, 0x199DBDB8, 0x4E8E05E4,
    0x4CE9AAFA, 0x50EAA0E4, 0x6165A9DC, 0xED984F91, 0x4005504E, 0x8B621E7E,
    0x480D539B, 0x00447713, 0x0313009C, 0x9FBB0D13, 0x12E2E599, 0xE738CC7E,
    0x1E948BC7, 0x5C7BD4DE, 0xD00F4A12, 0x8564C4DA, 0x2A930DE1, 0x7A6238CB,
    0x80CDE4C1, 0x23735360, 0x18B24651, 0x1FACD96C, 0x4CCFF4E9, 0xDDB712D9,
    0x41DC0FBA, 0xD22835D5, 0xC7BF670F, 0xD96D2E0D, 0x2EFD8F62, 0x4D153FBE,
    0x444A0CAC, 0xE9C45781, 0xCFC6238E, 0xD281017F, 0xE3E52623, 0x65BF4F30,
    0xF8E4BD99, 0x47C797B8, 0x682ADCE6, 0x74A92B58, 0x73659598, 0x3F17003D,
    0x4B57E8AF, 0x500B05CC, 0x1775FA3C, 0x9C0AE4F5, 0xE78675BB, 0xD1E15D71,
    0x81CF6509, 0xB991C6C7, 0x4B93374A, 0xD3559346, 0x98E72149, 0xE10094DE,
    0xC6658E1F, 0xE710D00C, 0x3DBBE6A9, 0xA437742A, 0xAEB527AB, 0x67FFE063,
    0xB39CAC9A, 0x5B0FFC3F, 0xC7A86530, 0x714BEC18, 0x16B840A4, 0x14F25615,
    0xAF07FFCA, 0x1E4AA34A, 0xBB4BA466, 0xBA32341F, 0x8C6FC33C, 0x74C796FF,
    0x37AC3A8A, 0xE2715AD9, 0x2FFF588F, 0x03DD26CB, 0x542B1322, 0x42C18B50,
    0xEBE5CA15, 0x0334095C, 0x3D7DD023, 0xF615FA5C, 0xD85EDB23, 0x03BF571D,
    0xA8429693, 0x7BC42C19, 0xA6F4DC90, 0x967906EE, 0x3B7808C9, 0xB1397EE5,
    0x1970C2E5, 0x00AACC91, 0x382F3D1F, 0x074EE5BB, 0x8FF35005, 0xE19EFD94,
    0x42B79C7E, 0xF011CC0F, 0x7C097704, 0x07D9B1BB, 0x63083E3B, 0x45E4990E,
    0x65A1C724, 0x3756AED0, 0x3E92042B, 0x886328B2, 0xCB530C96, 0xD25138C4,
    0xAC4F51BD, 0xE08E9A39, 0xB786C1AE, 0x8AB14190, 0xDC82224B, 0x674262B6,
    0x72603BCB, 0x16C59843, 0xCA9F4835, 0x0BF87349, 0x5F7666CB, 0xDA7D9C6D,
    0xAB217953, 0x8CBA680C, 0xB39A71D4, 0x157C25CB, 0xF295DFC7, 0x4E824CB0,
    0x2AB756F4, 0x1FAB2A49, 0x76B0E2EB, 0x0D19DF0F, 0xC901C9A7, 0x2B3DBE39,
    0x3E605B93, 0x663F0A25, 0x0E27E9DE, 0xCA6404CA, 0xC18771CA, 0x6E83AA1B,
    0xFE626BF5, 0xD252626E, 0x1761710E, 0xE82098EF, 0x179E901C, 0xADD2D644,
    0xC4B9BE11, 0x58C7D79B, 0x9CFC1AF2, 0x2703EF16, 0xBA31F652, 0xEA8FE5F3,
    0x2B28DD6C, 0xDA574227, 0x807DA7C6, 0x5B2D603A, 0xC4822439, 0xBE69EC7A,
    0x63969961, 0x681A19D1, 0x162BB034, 0xA5C5E861, 0x9331D5FA, 0x72225D4B,
    0xCFBC00D8, 0xDB87E81B, 0xDEE0DF1C, 0xC12421B1, 0x1987FC81, 0x32C71057,
    0xA52DA034, 0xB781F21C, 0x175CD601, 0x874F9A01, 0x09AF0AF4, 0x89988CFF,
    0xFFE1AF61, 0x95EB15C3, 0xB9E68EE4, 0xC7D70948
};

// Compute mask for a target average chunk size.
// WHY: We want ~avg_size chunks on average. The mask is chosen so that
// the probability of a boundary at any byte is 1/avg_size.
// mask = 2^ceil(log2(avg_size)) - 1
static uint32_t avg_to_mask(size_t avg) {
    uint32_t mask = 0;
    while ((size_t)(1u << __builtin_popcount(mask)) < avg)
        mask = (mask << 1) | 1;
    return mask;
}

FastCDCChunker::FastCDCChunker(const FastCDCConfig &cfg)
    : cfg_(cfg)
    , mask_s_(avg_to_mask(cfg.avg_size / 2))   // 50% size: find boundary earlier
    , mask_l_(avg_to_mask(cfg.avg_size * 2))    // 200% size: delay boundary
{}

// Find the cut point for one chunk starting at data[pos], given remaining bytes.
// Returns the size of this chunk.
size_t FastCDCChunker::cut(const uint8_t *data, size_t pos, size_t remaining) const {
    size_t n = remaining;
    if (n <= cfg_.min_size) return n; // too small to split

    // Cap at max_size
    size_t max_n = std::min(n, cfg_.max_size);

    // Phase 1: scan from min_size to avg_size using mask_l_ (harder to split)
    // WHY: We prefer not to split before min_size. Using mask_l_ (larger mask)
    // means fewer boundaries in this region.
    uint32_t hash = 0;
    size_t i = cfg_.min_size;
    size_t norm_size = std::min(cfg_.avg_size, max_n);

    for (; i < norm_size; i++) {
        hash = (hash << 1) + GEAR[data[pos + i]];
        if ((hash & mask_l_) == 0) return i;
    }

    // Phase 2: scan from avg_size to max_size using mask_s_ (easier to split)
    // WHY: After avg_size bytes we actively seek a boundary to prevent chunks
    // growing too large.
    for (; i < max_n; i++) {
        hash = (hash << 1) + GEAR[data[pos + i]];
        if ((hash & mask_s_) == 0) return i;
    }

    // No boundary found — return max_size (forced split)
    return max_n;
}

void FastCDCChunker::chunk(const uint8_t *data, size_t size, ChunkCallback cb) {
    size_t pos = 0;
    while (pos < size) {
        size_t chunk_size = cut(data, pos, size - pos);

        Chunk c;
        c.offset = pos;
        c.size   = chunk_size;
        c.data.assign(data + pos, data + pos + chunk_size);
        c.sha256 = sha256_hex(data + pos, chunk_size);

        cb(std::move(c));
        pos += chunk_size;
    }
}

// ── Fixed-size chunker ───────────────────────────────────────────────────────
FixedSizeChunker::FixedSizeChunker(size_t chunk_size)
    : chunk_size_(chunk_size) {}

void FixedSizeChunker::chunk(const uint8_t *data, size_t size, ChunkCallback cb) {
    size_t pos = 0;
    while (pos < size) {
        size_t csz = std::min(chunk_size_, size - pos);
        Chunk c;
        c.offset = pos;
        c.size   = csz;
        c.data.assign(data + pos, data + pos + csz);
        c.sha256 = sha256_hex(data + pos, csz);
        cb(std::move(c));
        pos += csz;
    }
}

// ── File reading with mmap ───────────────────────────────────────────────────
// WHY mmap for large files: read() copies data from kernel page cache to
// user buffer. mmap avoids this copy — we access the page cache directly.
// For large files, this reduces peak RSS and total memory bandwidth.
bool IChunker::chunk_file(const std::string &path, size_t mmap_threshold,
                           ChunkCallback cb) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;

    struct stat st{};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return false;
    }

    size_t file_size = static_cast<size_t>(st.st_size);
    if (file_size == 0) {
        close(fd);
        // Empty file: emit one zero-byte chunk so we record a manifest entry
        Chunk c;
        c.offset = 0; c.size = 0;
        c.sha256 = sha256_hex(nullptr, 0);
        cb(std::move(c));
        return true;
    }

    if (file_size >= mmap_threshold) {
        // Large file: map the entire file read-only into our address space
        // MAP_PRIVATE: copy-on-write; we won't modify it
        void *mapped = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (mapped == MAP_FAILED) return false;

        // Advise the kernel we'll access sequentially — enables readahead
        // WHY madvise: tells the kernel to prefetch pages ahead of our access,
        // avoiding page faults stalling the chunking loop.
        madvise(mapped, file_size, MADV_SEQUENTIAL);

        chunk(static_cast<const uint8_t*>(mapped), file_size, cb);
        munmap(mapped, file_size);
    } else {
        // Small file: read into a buffer
        std::vector<uint8_t> buf(file_size);
        ssize_t n = read(fd, buf.data(), file_size);
        close(fd);
        if (n < 0 || (size_t)n != file_size) return false;
        chunk(buf.data(), file_size, cb);
    }
    return true;
}

} // namespace cv
