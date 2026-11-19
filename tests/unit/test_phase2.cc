// tests/unit/test_phase2.cc — Phase 2: chunker, object store, manifest, versioner
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../../vendor/doctest.h"
#include "../../src/store/chunker.h"
#include "../../src/store/object_store.h"
#include "../../src/store/manifest.h"
#include "../../src/store/versioner.h"
#include "../../vendor/sha256.h"
#include "../../src/daemon/config.h"
#include "../../src/daemon/logger.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <random>
#include <chrono>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

// ─── Helpers ─────────────────────────────────────────────────────────────────

static std::string make_tmpdir() {
    char tmpl[] = "/tmp/cv_test_XXXXXX";
    char *d = mkdtemp(tmpl);
    REQUIRE(d != nullptr);
    return d;
}

static void write_file(const std::string &path, const std::string &content) {
    std::ofstream f(path, std::ios::binary);
    f.write(content.data(), (std::streamsize)content.size());
    REQUIRE(f.good());
}

static std::string read_file(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

static std::string random_bytes(size_t n) {
    std::mt19937_64 rng(42);
    std::string s(n, '\0');
    for (size_t i = 0; i < n; i++)
        s[i] = (char)(rng() & 0xFF);
    return s;
}

// ─── SHA-256 known-answer tests ───────────────────────────────────────────────
TEST_SUITE("SHA256") {

TEST_CASE("empty string") {
    // RFC 4634 KAT
    auto h = cv::sha256_hex(nullptr, 0);
    CHECK(h == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_CASE("abc") {
    const uint8_t data[] = {'a','b','c'};
    // Verified against: echo -n "abc" | openssl dgst -sha256
    const std::string expected = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    CHECK(cv::sha256_hex(data, 3) == expected);
    CHECK(cv::sha256_hex(std::string("abc")) == expected);
}

TEST_CASE("known 64-byte input fills one block") {
    std::string s(64, 'x');
    auto h1 = cv::sha256_hex(s);
    auto h2 = cv::sha256_hex(s); // deterministic
    CHECK(h1 == h2);
    CHECK(h1.size() == 64);
}

} // TEST_SUITE SHA256

// ─── FastCDC chunker tests ───────────────────────────────────────────────────
TEST_SUITE("FastCDC") {

TEST_CASE("small file fits in one chunk") {
    cv::FastCDCChunker chunker;
    std::string data = "Hello CrashVault";
    int count = 0;
    std::string got_sha;
    chunker.chunk(reinterpret_cast<const uint8_t*>(data.data()), data.size(),
        [&](cv::Chunk &&c) {
            count++;
            got_sha = c.sha256;
            CHECK(c.offset == 0);
            CHECK(c.size == data.size());
        });
    CHECK(count == 1);
    // Verify SHA matches manual computation
    CHECK(got_sha == cv::sha256_hex(data));
}

TEST_CASE("large random file produces multiple chunks") {
    cv::FastCDCChunker chunker;
    std::string data = random_bytes(512 * 1024); // 512 KiB
    size_t total = 0;
    int count = 0;
    std::string prev_sha;
    chunker.chunk(reinterpret_cast<const uint8_t*>(data.data()), data.size(),
        [&](cv::Chunk &&c) {
            count++;
            total += c.size;
            CHECK(c.sha256.size() == 64);
            CHECK(c.sha256 != prev_sha); // no two consecutive identical chunks
            prev_sha = c.sha256;
        });
    CHECK(count > 10);       // 512 KiB / 8 KiB avg = ~64 chunks
    CHECK(total == data.size()); // no bytes lost
}

TEST_CASE("editing one byte near middle changes only 1-2 chunks (CDC stability)") {
    cv::FastCDCChunker chunker;
    std::string base = random_bytes(256 * 1024);
    std::string edited = base;
    edited[128 * 1024] ^= 0xFF; // flip byte at the middle

    std::vector<std::string> base_shas, edit_shas;
    chunker.chunk(reinterpret_cast<const uint8_t*>(base.data()), base.size(),
        [&](cv::Chunk &&c) { base_shas.push_back(c.sha256); });
    chunker.chunk(reinterpret_cast<const uint8_t*>(edited.data()), edited.size(),
        [&](cv::Chunk &&c) { edit_shas.push_back(c.sha256); });

    // Count differing chunks
    size_t differ = 0;
    size_t n = std::min(base_shas.size(), edit_shas.size());
    for (size_t i = 0; i < n; i++)
        if (base_shas[i] != edit_shas[i]) differ++;

    // WHY ≤ 5: CDC guarantees only chunks spanning the edit boundary differ.
    // Perfect CDC would be 1; we allow up to 5 for boundary ripple.
    INFO("differ=" << differ << " of " << n << " chunks");
    CHECK(differ <= 5);
}

TEST_CASE("chunks are contiguous and cover all bytes") {
    cv::FastCDCChunker chunker;
    std::string data = random_bytes(100 * 1024);
    size_t expected_offset = 0;
    chunker.chunk(reinterpret_cast<const uint8_t*>(data.data()), data.size(),
        [&](cv::Chunk &&c) {
            CHECK(c.offset == expected_offset);
            expected_offset += c.size;
        });
    CHECK(expected_offset == data.size());
}

TEST_CASE("chunk file — real file on disk") {
    cv::FastCDCChunker chunker;
    auto tmpdir = make_tmpdir();
    std::string path = tmpdir + "/test.bin";
    std::string data = random_bytes(50 * 1024);
    write_file(path, data);

    size_t total = 0;
    bool ok = chunker.chunk_file(path, 1024*1024, [&](cv::Chunk &&c) {
        total += c.size;
        // Verify each chunk's SHA is correct
        CHECK(c.sha256 == cv::sha256_hex(c.data.data(), c.data.size()));
    });
    CHECK(ok);
    CHECK(total == data.size());
    fs::remove_all(tmpdir);
}

} // TEST_SUITE FastCDC

// ─── FixedSize chunker tests ─────────────────────────────────────────────────
TEST_SUITE("FixedSizeChunker") {

TEST_CASE("exact multiple of chunk size") {
    cv::FixedSizeChunker c(1024);
    std::string data(4096, 'A');
    int count = 0;
    c.chunk(reinterpret_cast<const uint8_t*>(data.data()), data.size(),
        [&](cv::Chunk &&ch) { count++; CHECK(ch.size == 1024); });
    CHECK(count == 4);
}

TEST_CASE("last chunk is smaller if not multiple") {
    cv::FixedSizeChunker c(1000);
    std::string data(3500, 'B');
    std::vector<size_t> sizes;
    c.chunk(reinterpret_cast<const uint8_t*>(data.data()), data.size(),
        [&](cv::Chunk &&ch) { sizes.push_back(ch.size); });
    REQUIRE(sizes.size() == 4);
    CHECK(sizes[0] == 1000);
    CHECK(sizes[1] == 1000);
    CHECK(sizes[2] == 1000);
    CHECK(sizes[3] == 500);
}

} // TEST_SUITE FixedSizeChunker

// ─── ObjectStore tests ───────────────────────────────────────────────────────
TEST_SUITE("ObjectStore") {

TEST_CASE("write, has, read round-trip") {
    auto tmpdir = make_tmpdir();
    cv::ObjectStore store(tmpdir);

    std::string data = "Hello, object store!";
    std::string sha = cv::sha256_hex(data);
    const auto *p = reinterpret_cast<const uint8_t*>(data.data());

    CHECK(!store.has_chunk(sha));
    bool written = store.write_chunk(sha, p, data.size());
    CHECK(written);
    CHECK(store.has_chunk(sha));

    std::vector<uint8_t> out;
    CHECK(store.read_chunk(sha, out));
    std::string got(out.begin(), out.end());
    CHECK(got == data);

    fs::remove_all(tmpdir);
}

TEST_CASE("dedup: writing same chunk twice returns false (no-op)") {
    auto tmpdir = make_tmpdir();
    cv::ObjectStore store(tmpdir);

    std::string data = "dedup me";
    std::string sha = cv::sha256_hex(data);
    const auto *p = reinterpret_cast<const uint8_t*>(data.data());

    CHECK(store.write_chunk(sha, p, data.size()));  // first write: true
    CHECK(!store.write_chunk(sha, p, data.size())); // second write: false (dedup hit)
    fs::remove_all(tmpdir);
}

TEST_CASE("verify_chunk: correct chunk passes, corrupted fails") {
    auto tmpdir = make_tmpdir();
    cv::ObjectStore store(tmpdir);

    std::string data = "verify me";
    std::string sha = cv::sha256_hex(data);
    const auto *p = reinterpret_cast<const uint8_t*>(data.data());
    store.write_chunk(sha, p, data.size());

    CHECK(store.verify_chunk(sha));

    // Corrupt the chunk file (must chmod first — chunks are stored read-only 0444)
    std::string cpath = store.chunk_path(sha);
    ::chmod(cpath.c_str(), 0644);
    std::ofstream f(cpath, std::ios::binary);
    f.write("CORRUPTED!!", 11);
    f.close();

    CHECK(!store.verify_chunk(sha)); // should fail — hash mismatch
    fs::remove_all(tmpdir);
}

TEST_CASE("chunk path layout: shard by first 2 hex chars") {
    auto tmpdir = make_tmpdir();
    cv::ObjectStore store(tmpdir);
    std::string sha = "abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890";
    std::string expected = tmpdir + "/objects/ab/cdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890";
    CHECK(store.chunk_path(sha) == expected);
    fs::remove_all(tmpdir);
}

} // TEST_SUITE ObjectStore

// ─── Manifest round-trip tests ───────────────────────────────────────────────
TEST_SUITE("Manifest") {

TEST_CASE("serialize and deserialize round-trip") {
    cv::Manifest m;
    m.id        = "test-uuid-1234";
    m.path      = "/project/main.cc";
    m.version   = 3;
    m.file_size = 12345;
    m.mtime     = 1700000000;
    m.mode      = 0644;
    m.sha256    = std::string(64, 'a');
    m.chunks    = {"aabb1122", "ccdd3344", "eeff5566"};
    m.timestamp = 1700000001;

    std::string json = m.to_json();
    cv::Manifest m2;
    CHECK(m2.from_json(json));
    CHECK(m2.id == m.id);
    CHECK(m2.path == m.path);
    CHECK(m2.version == m.version);
    CHECK(m2.file_size == m.file_size);
    CHECK(m2.sha256 == m.sha256);
    CHECK(m2.chunks == m.chunks);
}

TEST_CASE("from_json fails on invalid JSON") {
    cv::Manifest m;
    CHECK(!m.from_json("not json at all{{{"));
    CHECK(!m.from_json("{}"));  // missing required fields
}

TEST_CASE("ManifestStore write and read") {
    auto tmpdir = make_tmpdir();
    cv::ManifestStore ms(tmpdir);

    cv::Manifest m;
    m.id = cv::generate_uuid();
    m.path = "/test/file.txt";
    m.version = 1;
    m.file_size = 100;
    m.mtime = 1700000000;
    m.mode = 0644;
    m.sha256 = std::string(64, 'b');
    m.chunks = {"chunk1_sha256_placeholder_000000000000000000000000000000000000000000000000"};
    m.timestamp = time(nullptr);

    CHECK(ms.write_manifest(m));
    CHECK(ms.has_manifest(m.id));

    cv::Manifest m2;
    CHECK(ms.read_manifest(m.id, m2));
    CHECK(m2.path == m.path);
    CHECK(m2.version == m.version);
    fs::remove_all(tmpdir);
}

TEST_CASE("History: append and retrieve versions") {
    auto tmpdir = make_tmpdir();
    cv::ManifestStore ms(tmpdir);

    CHECK(ms.append_version("/project/file.txt", "uuid-v1"));
    CHECK(ms.append_version("/project/file.txt", "uuid-v2"));
    CHECK(ms.append_version("/project/file.txt", "uuid-v3"));

    auto h = ms.get_history("/project/file.txt");
    REQUIRE(h.size() == 3);
    CHECK(h[0] == "uuid-v1");
    CHECK(h[1] == "uuid-v2");
    CHECK(h[2] == "uuid-v3");
    fs::remove_all(tmpdir);
}

TEST_CASE("UUID generation: unique and correct format") {
    std::string u1 = cv::generate_uuid();
    std::string u2 = cv::generate_uuid();
    CHECK(u1 != u2);
    CHECK(u1.size() == 36); // 8-4-4-4-12 + 4 dashes
    CHECK(u1[8] == '-');
    CHECK(u1[13] == '-');
    CHECK(u1[18] == '-');
    CHECK(u1[23] == '-');
}

} // TEST_SUITE Manifest

// ─── Versioner end-to-end tests ───────────────────────────────────────────────
TEST_SUITE("Versioner") {

static cv::Config make_config(const std::string &store_root,
                               const std::string &watch_root) {
    cv::Config cfg;
    cfg.store_root = store_root;
    cfg.worker_count = 1;
    cfg.debounce_ms = 50;
    cfg.scheduling_policy = "fcfs";
    cfg.memory_cap_bytes = 64 * 1024 * 1024;
    cfg.mmap_threshold_bytes = 1024 * 1024;
    cfg.watched_roots = {watch_root};
    return cfg;
}

TEST_CASE("version a file and restore it") {
    auto storedir = make_tmpdir();
    auto watchdir = make_tmpdir();
    cv::Config cfg = make_config(storedir, watchdir);

    // Suppress logs in test
    cv::log_init("", cv::LogLevel::ERROR, false);

    cv::ObjectStore   obj(storedir);
    cv::ManifestStore ms(storedir);
    cv::Versioner     v(cfg, obj, ms);

    // Write a file
    std::string filepath = watchdir + "/hello.txt";
    std::string content  = "Hello, CrashVault version store!\n";
    write_file(filepath, content);

    // Version it
    auto result = v.process(filepath, false);
    CHECK(result.status == cv::VersionResult::Status::OK);
    CHECK(!result.manifest_id.empty());
    CHECK(result.chunks_written >= 1);

    // Check history
    auto hist = ms.get_history(filepath);
    REQUIRE(hist.size() == 1);

    // Corrupt the original
    write_file(filepath, "CORRUPTED DATA !!!!");

    // Restore
    CHECK(v.restore(result.manifest_id));
    CHECK(read_file(filepath) == content);

    fs::remove_all(storedir);
    fs::remove_all(watchdir);
}

TEST_CASE("idempotent: same content twice creates only 1 version") {
    auto storedir = make_tmpdir();
    auto watchdir = make_tmpdir();
    cv::Config cfg = make_config(storedir, watchdir);
    cv::log_init("", cv::LogLevel::ERROR, false);

    cv::ObjectStore   obj(storedir);
    cv::ManifestStore ms(storedir);
    cv::Versioner     v(cfg, obj, ms);

    std::string filepath = watchdir + "/same.txt";
    write_file(filepath, "constant content");

    auto r1 = v.process(filepath, false);
    auto r2 = v.process(filepath, false); // same content — should skip

    CHECK(r1.status == cv::VersionResult::Status::OK);
    CHECK(r2.status == cv::VersionResult::Status::SKIPPED_IGNORED);
    CHECK(ms.get_history(filepath).size() == 1);

    fs::remove_all(storedir);
    fs::remove_all(watchdir);
}

TEST_CASE("edit creates new version; dedup reduces written chunks") {
    auto storedir = make_tmpdir();
    auto watchdir = make_tmpdir();
    cv::Config cfg = make_config(storedir, watchdir);
    cv::log_init("", cv::LogLevel::ERROR, false);

    cv::ObjectStore   obj(storedir);
    cv::ManifestStore ms(storedir);
    cv::Versioner     v(cfg, obj, ms);

    // Large file: many chunks
    std::string filepath = watchdir + "/large.bin";
    std::string original = random_bytes(128 * 1024);
    write_file(filepath, original);

    auto r1 = v.process(filepath, false);
    CHECK(r1.status == cv::VersionResult::Status::OK);
    size_t chunks_v1 = r1.chunks_written;

    // Edit one byte — most chunks should be deduped
    std::string edited = original;
    edited[64 * 1024] ^= 0xFF;
    write_file(filepath, edited);

    auto r2 = v.process(filepath, false);
    CHECK(r2.status == cv::VersionResult::Status::OK);
    CHECK(r2.chunks_deduped > chunks_v1 / 2); // most chunks reused
    CHECK(r2.chunks_written <= 5);             // only boundary chunks changed

    // Both manifests recorded
    CHECK(ms.get_history(filepath).size() == 2);

    // Can restore to v1
    CHECK(v.restore(r1.manifest_id));
    CHECK(read_file(filepath) == original);

    fs::remove_all(storedir);
    fs::remove_all(watchdir);
}

TEST_CASE("versioning a deleted file returns SKIPPED_DELETED") {
    auto storedir = make_tmpdir();
    auto watchdir = make_tmpdir();
    cv::Config cfg = make_config(storedir, watchdir);
    cv::log_init("", cv::LogLevel::ERROR, false);

    cv::ObjectStore   obj(storedir);
    cv::ManifestStore ms(storedir);
    cv::Versioner     v(cfg, obj, ms);

    auto result = v.process("/nonexistent/path/file.txt", false);
    // Should be SKIPPED_DELETED (ENOENT during open)
    CHECK(result.status == cv::VersionResult::Status::SKIPPED_DELETED);

    fs::remove_all(storedir);
    fs::remove_all(watchdir);
}

} // TEST_SUITE Versioner


