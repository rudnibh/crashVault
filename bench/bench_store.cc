// bench/bench_store.cc — chunking + dedup evaluation
//
// Measures the two properties CrashVault claims:
//   1. FastCDC chunking throughput and boundary stability under small edits.
//   2. End-to-end deduplication: logical bytes written vs bytes actually
//      stored for repeated versions of the same file.
//
// Build:  cmake --build build --target bench_store
// Run:    ./build/bench_store

#include "../src/store/chunker.h"
#include "../src/store/object_store.h"
#include "../src/store/manifest.h"
#include "../src/store/wal.h"
#include "../src/store/versioner.h"
#include "../src/daemon/config.h"
#include "../src/daemon/logger.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

static std::string make_tmpdir() {
    char tmpl[] = "/tmp/cv_bench_XXXXXX";
    char *d = mkdtemp(tmpl);
    return d ? std::string(d) : std::string("/tmp");
}

static std::string random_bytes(size_t n) {
    std::mt19937_64 rng(12345);
    std::string s(n, '\0');
    for (size_t i = 0; i < n; i++) s[i] = (char)(rng() & 0xFF);
    return s;
}

static void write_file(const std::string &path, const std::string &data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(data.data(), (std::streamsize)data.size());
}

static void bench_chunkers() {
    std::printf("\nChunker comparison\n");
    std::printf("%-10s %-12s %8s %10s %12s %14s\n",
                "size", "chunker", "chunks", "avg_size", "chunk_ms", "changed_edit");

    cv::FastCDCChunker fast;
    cv::FixedSizeChunker fixed(8192);

    for (size_t size : {size_t(1) << 20, size_t(4) << 20, size_t(16) << 20}) {
        std::string base = random_bytes(size);
        // Insert bytes in the middle: this shifts every fixed-size boundary
        // after the edit, while CDC re-synchronises at the next content-defined
        // boundary. It is the workload CDC was designed for.
        std::string edited = base.substr(0, size / 2) +
                             "0123456789abcdef" +
                             base.substr(size / 2);

        auto run = [&](cv::IChunker &chunker, const std::string &data,
                       std::vector<std::string> *hashes) {
            auto t0 = std::chrono::steady_clock::now();
            size_t chunks = 0, total = 0;
            if (hashes) hashes->clear();
            chunker.chunk(reinterpret_cast<const uint8_t *>(data.data()), data.size(),
                          [&](cv::Chunk &&c) {
                              chunks++;
                              total += c.size;
                              if (hashes) hashes->push_back(c.sha256);
                          });
            auto t1 = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            return std::tuple<size_t, size_t, double>(chunks, total / std::max<size_t>(1, chunks), ms);
        };

        struct ChunkerEntry { const char *name; cv::IChunker *chunker; };
        const ChunkerEntry entries[] = {{"fastcdc", &fast}, {"fixed8k", &fixed}};
        for (const auto &entry : entries) {
            std::vector<std::string> a, b;
            auto [chunks, avg, ms] = run(*entry.chunker, base, &a);
            run(*entry.chunker, edited, &b);

            std::set<std::string> before(a.begin(), a.end());
            size_t changed = 0;
            for (auto &h : b) if (!before.count(h)) changed++;

            std::printf("%-10zu %-12s %8zu %10zu %12.2f %14zu\n",
                        size / 1024, entry.name, chunks, avg, ms, changed);
        }
    }
}

static void bench_versioning() {
    auto store = make_tmpdir();
    auto watch = make_tmpdir();

    cv::Config cfg;
    cfg.store_root = store;
    cfg.watched_roots = {watch};
    cfg.worker_count = 1;
    cfg.debounce_ms = 10;
    cv::log_init("", cv::LogLevel::ERROR, false);

    cv::ObjectStore   obj(store);
    cv::ManifestStore ms(store);
    cv::Wal           wal(store);
    cv::Versioner     versioner(cfg, obj, ms, wal);

    const size_t file_size = 4 * 1024 * 1024;
    const int versions = 10;
    std::string path = watch + "/bench.bin";
    std::string data = random_bytes(file_size);

    uint64_t logical_bytes = 0;
    uint64_t chunks_new = 0, chunks_total = 0;
    auto t0 = std::chrono::steady_clock::now();

    for (int v = 0; v < versions; v++) {
        if (v > 0) {
            // One-byte edit at a different offset each version.
            data[(size_t)v * 7919 % data.size()] ^= 0x5A;
        }
        write_file(path, data);
        auto r = versioner.process(path, false);
        if (r.status != cv::VersionResult::Status::OK) {
            std::fprintf(stderr, "versioning failed at v%d\n", v + 1);
            break;
        }
        logical_bytes += data.size();
        chunks_new += r.chunks_written;
        chunks_total += r.chunks_written + r.chunks_deduped;
    }

    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();

    uint64_t stored_bytes = 0;
    size_t chunk_files = 0;
    for (auto &entry : fs::recursive_directory_iterator(obj.objects_root())) {
        if (!entry.is_regular_file()) continue;
        std::string name = entry.path().filename().string();
        if (name.find(".tmp.") != std::string::npos) continue;
        stored_bytes += entry.file_size();
        chunk_files++;
    }

    double ratio = stored_bytes ? (double)logical_bytes / (double)stored_bytes : 0.0;
    std::printf("\nEnd-to-end versioning (FastCDC + object store)\n");
    std::printf("  versions:          %d\n", versions);
    std::printf("  logical bytes:     %llu\n", (unsigned long long)logical_bytes);
    std::printf("  stored bytes:      %llu\n", (unsigned long long)stored_bytes);
    std::printf("  dedup ratio:       %.2fx\n", ratio);
    std::printf("  chunks:            %llu new / %llu total (%.1f%% reused)\n",
                (unsigned long long)chunks_new, (unsigned long long)chunks_total,
                chunks_total ? 100.0 * (double)(chunks_total - chunks_new) /
                               (double)chunks_total : 0.0);
    std::printf("  chunk files:       %zu\n", chunk_files);
    std::printf("  elapsed:           %.3f s (%.1f MiB/s)\n", secs,
                secs > 0 ? (double)logical_bytes / (1024.0 * 1024.0) / secs : 0.0);

    fs::remove_all(store);
    fs::remove_all(watch);
}

int main() {
    std::printf("CrashVault benchmark\n");
    std::printf("====================\n");
    bench_chunkers();
    bench_versioning();
    return 0;
}
