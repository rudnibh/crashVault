// tests/unit/test_phase2.cc — Phase 2: chunker, object store, manifest, versioner
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../../vendor/doctest.h"
#include "../../src/store/chunker.h"
#include "../../src/store/object_store.h"
#include "../../src/store/manifest.h"
#include "../../src/store/versioner.h"
#include "../../src/store/wal.h"
#include "../../src/store/recovery.h"
#include "../../src/store/gc.h"
#include "../../src/pipeline/buffer_pool.h"
#include "../../src/daemon/failpoint.h"
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
#include <thread>
#include <atomic>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

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

static cv::Config make_store_config(const std::string &store_root,
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

TEST_CASE("chunk file — mmap path for large files") {
    cv::FastCDCChunker chunker;
    auto tmpdir = make_tmpdir();
    std::string path = tmpdir + "/large.bin";
    std::string data = random_bytes(2 * 1024 * 1024);
    write_file(path, data);

    size_t total = 0, chunks = 0;
    bool ok = chunker.chunk_file(path, 1024, [&](cv::Chunk &&c) {
        total += c.size;
        chunks++;
        CHECK(c.sha256 == cv::sha256_hex(c.data.data(), c.data.size()));
    });
    CHECK(ok);
    CHECK(chunks > 1);
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

    // Chunks are immutable once published.
    struct stat st{};
    REQUIRE(::stat(store.chunk_path(sha).c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0444);

    // Corrupt the chunk file (must chmod first — chunks are stored read-only 0444)
    std::string cpath = store.chunk_path(sha);
    ::chmod(cpath.c_str(), 0644);
    std::ofstream f(cpath, std::ios::binary);
    f.write("CORRUPTED!!", 11);
    f.close();

    CHECK(!store.verify_chunk(sha)); // should fail — hash mismatch
    std::vector<uint8_t> corrupt_out;
    CHECK(!store.read_chunk(sha, corrupt_out)); // restore must reject corrupt data
    fs::remove_all(tmpdir);
}

TEST_CASE("malformed chunk id is rejected without throwing") {
    auto tmpdir = make_tmpdir();
    cv::ObjectStore obj(tmpdir);
    std::vector<uint8_t> out;
    CHECK(!obj.read_chunk("short", out));
    CHECK(!obj.verify_chunk("short"));
    CHECK(!obj.has_chunk("short"));
    CHECK(obj.put_chunk("short", nullptr, 0) == cv::ObjectStore::WriteResult::Error);
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
    cv::Wal          wal(storedir);
    cv::Versioner    v(cfg, obj, ms, wal);

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
    cv::Wal          wal(storedir);
    cv::Versioner    v(cfg, obj, ms, wal);

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
    cv::Wal          wal(storedir);
    cv::Versioner    v(cfg, obj, ms, wal);

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
    cv::Wal          wal(storedir);
    cv::Versioner    v(cfg, obj, ms, wal);

    auto result = v.process("/nonexistent/path/file.txt", false);
    // Should be SKIPPED_DELETED (ENOENT during open)
    CHECK(result.status == cv::VersionResult::Status::SKIPPED_DELETED);

    fs::remove_all(storedir);
    fs::remove_all(watchdir);
}

} // TEST_SUITE Versioner



// ─── Phase 3: WAL tests ───────────────────────────────────────────────────────
TEST_SUITE("WAL") {

TEST_CASE("prepare creates a .wal file") {
    auto tmpdir = make_tmpdir();
    cv::Wal wal(tmpdir);

    uint64_t seq = wal.prepare("manifest-id-123", "/project/file.cc", 1,
                                {"sha256_chunk1", "sha256_chunk2"});
    CHECK(seq >= 0);

    // Check that a .wal file was created
    std::string wal_dir = tmpdir + "/wal";
    auto entries = wal.replay();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].entry.manifest_id == "manifest-id-123");
    CHECK(entries[0].entry.state == cv::WalEntry::State::PREPARE);
    CHECK(!entries[0].needs_history_update);

    fs::remove_all(tmpdir);
}

TEST_CASE("commit advances state to COMMIT") {
    auto tmpdir = make_tmpdir();
    cv::Wal wal(tmpdir);

    uint64_t seq = wal.prepare("mid-456", "/project/main.cc", 2,
                                {"sha_a", "sha_b"});
    CHECK(wal.commit(seq));

    auto entries = wal.replay();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].entry.state == cv::WalEntry::State::COMMIT);
    CHECK(entries[0].needs_history_update); // COMMIT without COMPLETE needs history

    fs::remove_all(tmpdir);
}

TEST_CASE("complete removes entry from replay") {
    auto tmpdir = make_tmpdir();
    cv::Wal wal(tmpdir);

    uint64_t seq = wal.prepare("mid-789", "/project/util.cc", 1, {});
    wal.commit(seq);
    CHECK(wal.complete(seq));

    // After complete, the entry should not appear in replay()
    auto entries = wal.replay();
    CHECK(entries.empty());

    fs::remove_all(tmpdir);
}

TEST_CASE("multiple operations tracked independently") {
    auto tmpdir = make_tmpdir();
    cv::Wal wal(tmpdir);

    uint64_t s1 = wal.prepare("m1", "/a.cc", 1, {"c1"});
    uint64_t s2 = wal.prepare("m2", "/b.cc", 1, {"c2"});
    wal.prepare("m3", "/c.cc", 1, {"c3"});

    wal.commit(s1);
    wal.complete(s1); // s1 done
    wal.commit(s2);   // s2 committed but not complete

    auto entries = wal.replay();
    CHECK(entries.size() == 2); // s2 (COMMIT) and s3 (PREPARE) are pending
    bool found_s2 = false, found_s3 = false;
    for (auto &e : entries) {
        if (e.entry.manifest_id == "m2") { found_s2 = true; CHECK(e.needs_history_update); }
        if (e.entry.manifest_id == "m3") { found_s3 = true; CHECK(!e.needs_history_update); }
    }
    CHECK(found_s2);
    CHECK(found_s3);

    fs::remove_all(tmpdir);
}

TEST_CASE("WAL serialize/deserialize round-trip") {
    cv::WalEntry e;
    e.seq         = 42;
    e.state       = cv::WalEntry::State::COMMIT;
    e.manifest_id = "test-manifest-uuid";
    e.file_path   = "/home/user/project/file.cc";
    e.version     = 7;
    e.chunk_shas  = {"sha1", "sha2", "sha3"};
    e.timestamp   = "2026-01-01T00:00:00Z";

    std::string json = e.to_json();
    cv::WalEntry e2;
    CHECK(e2.from_json(json));
    CHECK(e2.seq == 42);
    CHECK(e2.state == cv::WalEntry::State::COMMIT);
    CHECK(e2.manifest_id == "test-manifest-uuid");
    CHECK(e2.chunk_shas == e.chunk_shas);
}

TEST_CASE("WAL gc removes old done entries") {
    auto tmpdir = make_tmpdir();
    cv::Wal wal(tmpdir);

    // Create 10 completed entries
    for (int i = 0; i < 10; i++) {
        uint64_t seq = wal.prepare("mid-" + std::to_string(i), "/f" + std::to_string(i), 1, {});
        wal.commit(seq);
        wal.complete(seq);
    }

    // All 10 are completed; replay should return empty
    CHECK(wal.replay().empty());

    // GC keeping only 3
    wal.gc(3);

    // Count .wal.done files remaining
    size_t count = 0;
    std::string wal_dir = tmpdir + "/wal";
    for (auto &p : fs::directory_iterator(wal_dir)) {
        if (p.path().extension() == ".done") count++;
    }
    CHECK(count == 3);

    fs::remove_all(tmpdir);
}

TEST_CASE("versioner uses WAL: full round-trip with WAL verification") {
    auto storedir = make_tmpdir();
    auto watchdir = make_tmpdir();
    cv::Config cfg;
    cfg.store_root        = storedir;
    cfg.worker_count      = 1;
    cfg.debounce_ms       = 50;
    cfg.scheduling_policy = "fcfs";
    cfg.memory_cap_bytes  = 64 * 1024 * 1024;
    cfg.mmap_threshold_bytes = 1024 * 1024;
    cfg.watched_roots     = {watchdir};
    cv::log_init("", cv::LogLevel::ERROR, false);

    cv::ObjectStore   obj(storedir);
    cv::ManifestStore ms(storedir);
    cv::Wal           wal(storedir);
    cv::Versioner     v(cfg, obj, ms, wal);

    std::string filepath = watchdir + "/test.txt";
    write_file(filepath, "WAL integration test content\n");

    auto result = v.process(filepath, false);
    CHECK(result.status == cv::VersionResult::Status::OK);

    // After successful versioning, WAL should have no pending entries
    // (all three phases: prepare, commit, complete were executed)
    auto pending = wal.replay();
    CHECK(pending.empty()); // complete was called successfully

    fs::remove_all(storedir);
    fs::remove_all(watchdir);
}

} // TEST_SUITE WAL

// ─── Phase 3b: crash recovery ────────────────────────────────────────────────
TEST_SUITE("Recovery") {

TEST_CASE("COMMIT without COMPLETE re-appends history") {
    auto tmpdir = make_tmpdir();
    cv::ManifestStore ms(tmpdir);
    cv::Wal wal(tmpdir);

    cv::Manifest m;
    m.id = "committed-manifest";
    m.path = "/project/a.cc";
    m.version = 1;
    m.file_size = 5;
    m.mtime = 1700000000;
    m.mode = 0644;
    m.sha256 = std::string(64, 'a');
    m.timestamp = 1700000001;
    REQUIRE(ms.write_manifest(m));

    uint64_t seq = wal.prepare(m.id, m.path, 1, {});
    REQUIRE(wal.commit(seq));

    auto stats = cv::recover_wal(wal, ms);
    CHECK(stats.pending == 1);
    CHECK(stats.repaired == 1);
    CHECK(stats.discarded == 0);
    CHECK(stats.failed == 0);

    auto hist = ms.get_history(m.path);
    REQUIRE(hist.size() == 1);
    CHECK(hist[0] == m.id);
    CHECK(wal.replay().empty());

    fs::remove_all(tmpdir);
}

TEST_CASE("PREPARE-only entry is discarded safely") {
    auto tmpdir = make_tmpdir();
    cv::ManifestStore ms(tmpdir);
    cv::Wal wal(tmpdir);

    uint64_t seq = wal.prepare("uncommitted", "/project/b.cc", 1, {});
    REQUIRE(seq > 0);

    auto stats = cv::recover_wal(wal, ms);
    CHECK(stats.pending == 1);
    CHECK(stats.discarded == 1);
    CHECK(stats.repaired == 0);

    CHECK(ms.get_history("/project/b.cc").empty());
    CHECK(wal.replay().empty());

    fs::remove_all(tmpdir);
}

TEST_CASE("PREPARE entry with a durable manifest is completed") {
    auto tmpdir = make_tmpdir();
    cv::ManifestStore ms(tmpdir);
    cv::Wal wal(tmpdir);

    cv::Manifest m;
    m.id = "manifest-before-commit";
    m.path = "/project/crash.cc";
    m.version = 1;
    m.sha256 = std::string(64, 'b');
    REQUIRE(ms.write_manifest(m));

    // Crash point: manifest written, wal.commit() never reached.
    uint64_t seq = wal.prepare(m.id, m.path, 1, {});
    REQUIRE(seq > 0);

    auto stats = cv::recover_wal(wal, ms);
    CHECK(stats.pending == 1);
    CHECK(stats.repaired == 1);
    CHECK(stats.discarded == 0);
    CHECK(ms.get_history(m.path) == std::vector<std::string>{m.id});
    CHECK(wal.replay().empty());

    fs::remove_all(tmpdir);
}

TEST_CASE("replay is idempotent when history already contains manifest") {
    auto tmpdir = make_tmpdir();
    cv::ManifestStore ms(tmpdir);
    cv::Wal wal(tmpdir);

    cv::Manifest m;
    m.id = "already-in-history";
    m.path = "/project/c.cc";
    m.version = 1;
    m.sha256 = std::string(64, 'c');
    REQUIRE(ms.write_manifest(m));
    REQUIRE(ms.append_version(m.path, m.id));

    uint64_t seq = wal.prepare(m.id, m.path, 1, {});
    REQUIRE(wal.commit(seq));

    auto stats = cv::recover_wal(wal, ms);
    CHECK(stats.repaired == 1);
    CHECK(ms.get_history(m.path).size() == 1); // no duplicate

    fs::remove_all(tmpdir);
}

} // TEST_SUITE Recovery

// ─── Phase 4: snapshots, verification, retention helpers ─────────────────────
TEST_SUITE("ManifestStore advanced") {

TEST_CASE("create_snapshot captures latest version per file") {
    auto storedir = make_tmpdir();
    auto watchdir = make_tmpdir();
    cv::Config cfg = make_store_config(storedir, watchdir);
    cv::log_init("", cv::LogLevel::ERROR, false);

    cv::ObjectStore   obj(storedir);
    cv::ManifestStore ms(storedir);
    cv::Wal           wal(storedir);
    cv::Versioner     v(cfg, obj, ms, wal);

    std::string f1 = watchdir + "/one.txt";
    std::string f2 = watchdir + "/two.txt";
    write_file(f1, "version one");
    auto r1 = v.process(f1, false);
    CHECK(r1.status == cv::VersionResult::Status::OK);
    write_file(f2, "version two");
    auto r2 = v.process(f2, false);
    CHECK(r2.status == cv::VersionResult::Status::OK);

    cv::Snapshot snap;
    REQUIRE(ms.create_snapshot(watchdir, snap));
    CHECK(snap.root == watchdir);
    CHECK(snap.files.size() == 2);
    CHECK(snap.files[f1] == r1.manifest_id);
    CHECK(snap.files[f2] == r2.manifest_id);
    CHECK(ms.list_snapshots().size() == 1);

    fs::remove_all(storedir);
    fs::remove_all(watchdir);
}

TEST_CASE("set_history can truncate and delete paths") {
    auto tmpdir = make_tmpdir();
    cv::ManifestStore ms(tmpdir);

    REQUIRE(ms.append_version("/a", "m1"));
    REQUIRE(ms.append_version("/a", "m2"));
    REQUIRE(ms.append_version("/b", "m3"));
    REQUIRE(ms.list_history_paths().size() == 2);

    REQUIRE(ms.set_history("/a", {"m2"}));
    CHECK(ms.get_history("/a") == std::vector<std::string>{"m2"});

    REQUIRE(ms.set_history("/b", {}));
    CHECK(ms.get_history("/b").empty());
    CHECK(ms.list_history_paths() == std::vector<std::string>{"/a"});

    fs::remove_all(tmpdir);
}

TEST_CASE("verify_all detects a corrupted chunk") {
    auto tmpdir = make_tmpdir();
    cv::ObjectStore obj(tmpdir);
    cv::ManifestStore ms(tmpdir);

    std::string data = "chunk to corrupt";
    std::string sha = cv::sha256_hex(data);
    REQUIRE(obj.write_chunk(sha, reinterpret_cast<const uint8_t*>(data.data()),
                            data.size()));

    cv::Manifest m;
    m.id = "verify-me";
    m.path = "/project/corrupt.cc";
    m.version = 1;
    m.sha256 = std::string(64, 'd');
    m.chunks = {sha};
    REQUIRE(ms.write_manifest(m));

    CHECK(ms.verify_all(obj.objects_root()).empty());

    std::string cpath = obj.chunk_path(sha);
    REQUIRE(::chmod(cpath.c_str(), 0644) == 0);
    {
        std::ofstream f(cpath, std::ios::binary | std::ios::trunc);
        f << "tampered";
    }
    auto problems = ms.verify_all(obj.objects_root());
    CHECK(!problems.empty());
    CHECK(problems[0].find("hash mismatch") != std::string::npos);

    fs::remove_all(tmpdir);
}

TEST_CASE("verify_all detects corrupt history JSON") {
    auto tmpdir = make_tmpdir();
    cv::ManifestStore ms(tmpdir);
    write_file(tmpdir + "/meta/file_history.json", "{ not valid json");

    auto problems = ms.verify_all(tmpdir + "/objects");
    CHECK(!problems.empty());
    bool found = false;
    for (const auto &p : problems) {
        if (p.find("history") != std::string::npos) found = true;
    }
    CHECK(found);

    fs::remove_all(tmpdir);
}

TEST_CASE("delete_manifest removes it from disk") {
    auto tmpdir = make_tmpdir();
    cv::ManifestStore ms(tmpdir);
    cv::Manifest m;
    m.id = "to-delete";
    m.path = "/x";
    m.version = 1;
    m.sha256 = std::string(64, 'e');
    REQUIRE(ms.write_manifest(m));
    CHECK(ms.has_manifest("to-delete"));
    CHECK(ms.delete_manifest("to-delete"));
    CHECK(!ms.has_manifest("to-delete"));
    fs::remove_all(tmpdir);
}

} // TEST_SUITE ManifestStore advanced

// ─── Phase 5: GC, snapshots, restore helpers ─────────────────────────────────
TEST_SUITE("GC and snapshot restore") {

TEST_CASE("retention trims history and reclaims unreferenced chunks") {
    auto storedir = make_tmpdir();
    auto watchdir = make_tmpdir();
    cv::Config cfg = make_store_config(storedir, watchdir);
    cv::log_init("", cv::LogLevel::ERROR, false);

    cv::ObjectStore   obj(storedir);
    cv::ManifestStore ms(storedir);
    cv::Wal           wal(storedir);
    cv::Versioner     v(cfg, obj, ms, wal);

    std::string filepath = watchdir + "/grow.bin";
    std::vector<std::string> versions;
    std::string last_content;
    for (int i = 0; i < 4; i++) {
        last_content = random_bytes(64 * 1024) + std::to_string(i);
        write_file(filepath, last_content);
        auto r = v.process(filepath, false);
        REQUIRE(r.status == cv::VersionResult::Status::OK);
        versions.push_back(r.manifest_id);
    }
    REQUIRE(ms.get_history(filepath).size() == 4);

    auto stats = cv::run_gc(ms, obj, 1, 30);
    CHECK(stats.history_entries_trimmed == 3);
    auto hist = ms.get_history(filepath);
    REQUIRE(hist.size() == 1);
    CHECK(hist[0] == versions.back());

    // Old manifests are gone; only the newest remains.
    CHECK(!ms.has_manifest(versions[0]));
    CHECK(!ms.has_manifest(versions[1]));
    CHECK(!ms.has_manifest(versions[2]));
    CHECK(ms.has_manifest(versions[3]));
    CHECK(stats.chunks_deleted > 0);
    CHECK(stats.bytes_reclaimed > 0);

    // The surviving manifest is still fully restorable.
    write_file(filepath, "overwritten");
    CHECK(v.restore(versions.back()));
    CHECK(read_file(filepath) == last_content);

    fs::remove_all(storedir);
    fs::remove_all(watchdir);
}

TEST_CASE("GC keeps manifests referenced by a snapshot") {
    auto storedir = make_tmpdir();
    auto watchdir = make_tmpdir();
    cv::Config cfg = make_store_config(storedir, watchdir);
    cv::log_init("", cv::LogLevel::ERROR, false);

    cv::ObjectStore   obj(storedir);
    cv::ManifestStore ms(storedir);
    cv::Wal           wal(storedir);
    cv::Versioner     v(cfg, obj, ms, wal);

    std::string filepath = watchdir + "/keep.txt";
    write_file(filepath, "old content");
    auto old = v.process(filepath, false);
    REQUIRE(old.status == cv::VersionResult::Status::OK);

    cv::Snapshot snap;
    REQUIRE(ms.create_snapshot(watchdir, snap));

    write_file(filepath, "new content");
    auto newer = v.process(filepath, false);
    REQUIRE(newer.status == cv::VersionResult::Status::OK);

    cv::run_gc(ms, obj, 1, 30); // history keeps 1, but snapshot protects old
    CHECK(ms.has_manifest(old.manifest_id));
    CHECK(ms.has_manifest(newer.manifest_id));

    fs::remove_all(storedir);
    fs::remove_all(watchdir);
}

TEST_CASE("GC removes stale metadata temp files") {
    auto storedir = make_tmpdir();
    cv::ObjectStore   obj(storedir);
    cv::ManifestStore ms(storedir);

    std::vector<std::string> temps = {
        storedir + "/manifests/orphan.json.tmp.ABCDEF",
        storedir + "/snapshots/orphan.json.tmp.ABCDEF",
        storedir + "/meta/file_history.json.tmp.ABCDEF"
    };
    for (const auto &p : temps) write_file(p, "partial");

    struct utimbuf old{};
    old.actime = old.modtime = time(nullptr) - 7200;
    for (const auto &p : temps) REQUIRE(utime(p.c_str(), &old) == 0);

    auto stats = cv::run_gc(ms, obj, 10, 30);
    CHECK(stats.temp_files_deleted == temps.size());
    for (const auto &p : temps) CHECK(!fs::exists(p));

    fs::remove_all(storedir);
}

TEST_CASE("restore_snapshot writes an isolated copy") {
    auto storedir = make_tmpdir();
    auto watchdir = make_tmpdir();
    auto restoredir = make_tmpdir();
    cv::Config cfg = make_store_config(storedir, watchdir);
    cv::log_init("", cv::LogLevel::ERROR, false);

    cv::ObjectStore   obj(storedir);
    cv::ManifestStore ms(storedir);
    cv::Wal           wal(storedir);
    cv::Versioner     v(cfg, obj, ms, wal);

    std::string f1 = watchdir + "/a.txt";
    std::string f2 = watchdir + "/sub/b.txt";
    fs::create_directories(watchdir + "/sub");
    write_file(f1, "snapshot A");
    write_file(f2, "snapshot B");
    REQUIRE(v.process(f1, false).status == cv::VersionResult::Status::OK);
    REQUIRE(v.process(f2, false).status == cv::VersionResult::Status::OK);

    cv::Snapshot snap;
    REQUIRE(ms.create_snapshot(watchdir, snap));

    // Change both source files after the snapshot.
    write_file(f1, "changed A");
    write_file(f2, "changed B");

    REQUIRE(v.restore_snapshot(snap, restoredir));
    CHECK(read_file(restoredir + "/a.txt") == "snapshot A");
    CHECK(read_file(restoredir + "/sub/b.txt") == "snapshot B");
    // Original files must not have been touched.
    CHECK(read_file(f1) == "changed A");

    fs::remove_all(storedir);
    fs::remove_all(watchdir);
    fs::remove_all(restoredir);
}

TEST_CASE("restore_latest picks the newest manifest") {
    auto storedir = make_tmpdir();
    auto watchdir = make_tmpdir();
    cv::Config cfg = make_store_config(storedir, watchdir);
    cv::log_init("", cv::LogLevel::ERROR, false);

    cv::ObjectStore   obj(storedir);
    cv::ManifestStore ms(storedir);
    cv::Wal           wal(storedir);
    cv::Versioner     v(cfg, obj, ms, wal);

    std::string path = watchdir + "/latest.txt";
    write_file(path, "one");
    REQUIRE(v.process(path, false).status == cv::VersionResult::Status::OK);
    write_file(path, "two");
    REQUIRE(v.process(path, false).status == cv::VersionResult::Status::OK);

    write_file(path, "garbage");
    REQUIRE(v.restore_latest(path));
    CHECK(read_file(path) == "two");

    fs::remove_all(storedir);
    fs::remove_all(watchdir);
}

TEST_CASE("failpoint_enabled reads CV_FAILPOINT") {
    unsetenv("CV_FAILPOINT");
    CHECK(!cv::failpoint_enabled("wal_after_commit"));
    setenv("CV_FAILPOINT", "wal_after_commit", 1);
    CHECK(cv::failpoint_enabled("wal_after_commit"));
    CHECK(!cv::failpoint_enabled("wal_after_prepare"));
    unsetenv("CV_FAILPOINT");
}

TEST_CASE("GC and versioning are serialized by the store gate") {
    auto storedir = make_tmpdir();
    auto watchdir = make_tmpdir();
    cv::Config cfg = make_store_config(storedir, watchdir);
    cv::log_init("", cv::LogLevel::ERROR, false);

    cv::ObjectStore   obj(storedir);
    cv::ManifestStore ms(storedir);
    cv::Wal           wal(storedir);
    cv::Versioner     v(cfg, obj, ms, wal);

    std::string path = watchdir + "/concurrent.bin";
    std::atomic<bool> stop{false};
    std::atomic<int>  gc_runs{0};

    std::thread gc_thread([&] {
        while (!stop.load()) {
            cv::run_gc(ms, obj, 2, 30);
            gc_runs.fetch_add(1);
        }
    });

    // Give the GC thread a chance to enter its loop before the
    // versioning workload starts, so the test really overlaps them.
    for (int i = 0; i < 1000 && gc_runs.load() == 0; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    int failures = 0;
    for (int i = 0; i < 20; i++) {
        write_file(path, std::to_string(i) + random_bytes(128 * 1024));
        auto r = v.process(path, false);
        if (r.status != cv::VersionResult::Status::OK) failures++;
    }
    stop.store(true);
    gc_thread.join();

    CHECK(failures == 0);
    CHECK(gc_runs.load() > 0);
    CHECK(ms.verify_all(obj.objects_root()).empty());

    fs::remove_all(storedir);
    fs::remove_all(watchdir);
}

} // TEST_SUITE GC and snapshot restore

// ─── Phase 6: bounded-memory read budget ─────────────────────────────────────
TEST_SUITE("BufferPool") {

TEST_CASE("leases enforce the byte budget") {
    cv::BufferPool pool(1000);

    auto a = pool.acquire(600);
    CHECK(pool.in_use() == 600);
    CHECK(pool.available() == 400);

    {
        auto b = pool.acquire(400);
        CHECK(pool.available() == 0);
    }
    CHECK(pool.available() == 400);

    a.release();
    CHECK(pool.in_use() == 0);

    // An oversized request reserves the whole budget and runs alone.
    auto big = pool.acquire(5000);
    CHECK(pool.in_use() == pool.capacity());
    big.release();
    CHECK(pool.in_use() == 0);
}

TEST_CASE("acquire blocks until memory is released") {
    cv::BufferPool pool(100);
    auto held = pool.acquire(100);

    std::atomic<bool> acquired{false};
    std::thread t([&] {
        auto lease = pool.acquire(100);
        acquired.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(!acquired.load());

    held.release();
    t.join();
    CHECK(acquired.load());
    CHECK(pool.in_use() == 0);
}

TEST_CASE("large file works with a tiny read budget") {
    auto storedir = make_tmpdir();
    auto watchdir = make_tmpdir();
    cv::Config cfg = make_store_config(storedir, watchdir);
    cfg.memory_cap_bytes = 1024; // deliberately smaller than the file
    cv::log_init("", cv::LogLevel::ERROR, false);

    cv::ObjectStore   obj(storedir);
    cv::ManifestStore ms(storedir);
    cv::Wal           wal(storedir);
    cv::Versioner     v(cfg, obj, ms, wal);

    std::string path = watchdir + "/big.bin";
    write_file(path, random_bytes(2 * 1024 * 1024));

    auto r = v.process(path, false);
    CHECK(r.status == cv::VersionResult::Status::OK);
    CHECK(r.chunks_written > 1);
    CHECK(v.read_budget_in_use() == 0); // lease released on return

    fs::remove_all(storedir);
    fs::remove_all(watchdir);
}

} // TEST_SUITE BufferPool
