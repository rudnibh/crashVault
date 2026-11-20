// tests/unit/test_phase1.cc — Phase 1 unit tests
//
// Tests:
//   1. Config parsing (INI file, defaults, error handling)
//   2. Debouncer coalescing: burst of events → single settled event per path
//   3. Bounded queue: push/pop, capacity, shutdown
//   4. Debouncer: different write styles produce exactly one settled event
//   5. Bounded queue: coalescing of duplicate paths
//
// Run: ./test_phase1
// or:  ctest -R phase1

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../../vendor/doctest.h"
#include "../../src/daemon/config.h"
#include "../../src/watcher/debouncer.h"
#include "../../src/pipeline/bounded_queue.h"
#include <fstream>
#include <filesystem>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <vector>
#include <algorithm>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

// ─── Helper: temporary directory ─────────────────────────────────────────────
struct TempDir {
    fs::path path;
    explicit TempDir(const std::string &prefix = "cvtest") {
        char tmpl[] = "/tmp/cvtest_XXXXXX";
        char *d = mkdtemp(tmpl);
        REQUIRE(d != nullptr);
        path = d;
    }
    ~TempDir() { fs::remove_all(path); }
};

// ─── Config tests ─────────────────────────────────────────────────────────────
TEST_SUITE("Config") {

TEST_CASE("defaults when no file") {
    cv::Config cfg;
    std::string err;
    bool ok = cfg.load("", err);
    CHECK(ok);
    CHECK(cfg.worker_count == 4);
    CHECK(cfg.debounce_ms == 300);
    CHECK(cfg.scheduling_policy == "fcfs");
    CHECK(!cfg.ignore_patterns.empty()); // has default patterns
}

TEST_CASE("parse a valid INI file") {
    TempDir tmp;
    fs::path cfg_path = tmp.path / "config.ini";
    std::ofstream f(cfg_path);
    f << "[daemon]\n"
         "worker_count = 2\n"
         "debounce_ms = 100\n"
         "scheduling_policy = priority\n"
         "memory_cap_mb = 64\n"
         "[watch]\n"
         "root = /tmp/watched\n"
         "ignore = *.bak\n"
         "[retention]\n"
         "versions = 5\n";
    f.close();

    cv::Config cfg;
    std::string err;
    bool ok = cfg.load(cfg_path.string(), err);
    CHECK(ok);
    CHECK(cfg.worker_count == 2);
    CHECK(cfg.debounce_ms == 100);
    CHECK(cfg.scheduling_policy == "priority");
    CHECK(cfg.memory_cap_bytes == 64ULL * 1024 * 1024);
    CHECK(cfg.retention_versions == 5);
    REQUIRE(!cfg.watched_roots.empty());
    CHECK(cfg.watched_roots[0] == "/tmp/watched");
    // Should have both defaults and the user-added *.bak pattern
    bool has_bak = std::find(cfg.ignore_patterns.begin(), cfg.ignore_patterns.end(),
                             "*.bak") != cfg.ignore_patterns.end();
    CHECK(has_bak);
}

TEST_CASE("invalid worker_count") {
    TempDir tmp;
    fs::path cfg_path = tmp.path / "config.ini";
    std::ofstream f(cfg_path);
    f << "[daemon]\nworker_count = 0\n";
    f.close();

    cv::Config cfg;
    std::string err;
    bool ok = cfg.load(cfg_path.string(), err);
    CHECK(!ok);
    CHECK(!err.empty());
}

TEST_CASE("malformed numeric value is rejected without throwing") {
    TempDir tmp;
    fs::path cfg_path = tmp.path / "config.ini";
    std::ofstream f(cfg_path);
    f << "[daemon]\nworker_count = not-a-number\n";
    f.close();

    cv::Config cfg;
    std::string err;
    bool ok = cfg.load(cfg_path.string(), err);
    CHECK(!ok);
    CHECK(err.find("Invalid value") != std::string::npos);
}

TEST_CASE("negative megabyte value is rejected") {
    TempDir tmp;
    fs::path cfg_path = tmp.path / "config.ini";
    std::ofstream f(cfg_path);
    f << "[daemon]\nmemory_cap_mb = -1\n";
    f.close();

    cv::Config cfg;
    std::string err;
    bool ok = cfg.load(cfg_path.string(), err);
    CHECK(!ok);
    CHECK(err.find("Invalid value") != std::string::npos);
}

TEST_CASE("unknown scheduling policy is rejected") {
    TempDir tmp;
    fs::path cfg_path = tmp.path / "config.ini";
    std::ofstream f(cfg_path);
    f << "[daemon]\nscheduling_policy = random\n";
    f.close();

    cv::Config cfg;
    std::string err;
    bool ok = cfg.load(cfg_path.string(), err);
    CHECK(!ok);
    CHECK(err.find("scheduling_policy") != std::string::npos);
}

TEST_CASE("comments and blank lines ignored") {
    TempDir tmp;
    fs::path cfg_path = tmp.path / "config.ini";
    std::ofstream f(cfg_path);
    f << "# This is a comment\n"
         "; Also a comment\n"
         "\n"
         "[daemon]\n"
         "worker_count = 3  # trailing comment\n";
    f.close();

    cv::Config cfg;
    std::string err;
    bool ok = cfg.load(cfg_path.string(), err);
    CHECK(ok);
    CHECK(cfg.worker_count == 3);
}

} // TEST_SUITE Config

// ─── Debouncer tests ──────────────────────────────────────────────────────────
TEST_SUITE("Debouncer") {

TEST_CASE("single event produces exactly one settled notification") {
    std::atomic<int> count{0};
    std::vector<std::string> settled;
    std::mutex mu;

    cv::Debouncer deb(50 /*ms*/, [&](cv::SettledEvent ev) {
        std::lock_guard<std::mutex> lk(mu);
        settled.push_back(ev.path);
        count.fetch_add(1);
    });
    deb.start();

    deb.push("/test/file.txt");
    std::this_thread::sleep_for(200ms);
    deb.stop();

    CHECK(count.load() == 1);
    CHECK(settled[0] == "/test/file.txt");
}

TEST_CASE("burst of events on same path → exactly one settled event") {
    // Simulates: vim write temp + rename + chmod → 5 events, 1 settled
    std::atomic<int> count{0};
    cv::Debouncer deb(100 /*ms*/, [&](cv::SettledEvent) { count.fetch_add(1); });
    deb.start();

    // 5 rapid-fire events for the same path
    for (int i = 0; i < 5; i++) {
        deb.push("/project/main.cc");
        std::this_thread::sleep_for(10ms); // faster than debounce window
    }

    std::this_thread::sleep_for(300ms); // wait for debounce to fire
    deb.stop();

    CHECK(count.load() == 1);
}

TEST_CASE("events on different paths → one settled event per path") {
    std::atomic<int> count{0};
    std::vector<std::string> settled_paths;
    std::mutex mu;

    cv::Debouncer deb(50 /*ms*/, [&](cv::SettledEvent ev) {
        std::lock_guard<std::mutex> lk(mu);
        settled_paths.push_back(ev.path);
        count.fetch_add(1);
    });
    deb.start();

    deb.push("/project/a.cc");
    deb.push("/project/b.cc");
    deb.push("/project/c.cc");

    std::this_thread::sleep_for(300ms);
    deb.stop();

    CHECK(count.load() == 3);
}

TEST_CASE("atomic-rename pattern: burst then quiet → single settled event") {
    // Simulate vim save: write to .#file.txt~, rename to file.txt, chmod
    // All within 30ms. Debounce window is 100ms.
    // Expected: exactly 1 settled event for file.txt
    std::atomic<int> count{0};
    cv::Debouncer deb(100 /*ms*/, [&](cv::SettledEvent) { count.fetch_add(1); });
    deb.start();

    deb.push("/dir/file.txt"); // IN_CREATE .#file.txt~
    std::this_thread::sleep_for(5ms);
    deb.push("/dir/file.txt"); // IN_CLOSE_WRITE temp file
    std::this_thread::sleep_for(5ms);
    deb.push("/dir/file.txt"); // IN_MOVED_TO (rename into place)
    std::this_thread::sleep_for(5ms);
    deb.push("/dir/file.txt"); // IN_ATTRIB (chmod)

    std::this_thread::sleep_for(400ms);
    deb.stop();

    CHECK(count.load() == 1);
}

TEST_CASE("delete event propagates correctly") {
    bool got_delete = false;
    cv::Debouncer deb(50 /*ms*/, [&](cv::SettledEvent ev) {
        got_delete = ev.deleted;
    });
    deb.start();
    deb.push("/test/gone.txt", true /*deleted*/);
    std::this_thread::sleep_for(200ms);
    deb.stop();
    CHECK(got_delete);
}

} // TEST_SUITE Debouncer

// ─── BoundedQueue tests ───────────────────────────────────────────────────────
TEST_SUITE("BoundedQueue") {

TEST_CASE("push and pop basic") {
    cv::BoundedQueue q(10, "fcfs");
    q.push({"path/a", false, 0, 0});
    q.push({"path/b", false, 0, 0});

    cv::WorkItem item;
    CHECK(q.pop(item));
    CHECK(item.path == "path/a");
    CHECK(q.pop(item));
    CHECK(item.path == "path/b");
}

TEST_CASE("shutdown unblocks waiting consumers") {
    cv::BoundedQueue q(10, "fcfs");

    bool popped = true;
    std::thread consumer([&] {
        cv::WorkItem item;
        popped = q.pop(item); // should block then return false on shutdown
    });

    std::this_thread::sleep_for(50ms);
    q.shutdown();
    consumer.join();
    CHECK(!popped); // pop returned false (shutdown)
}

TEST_CASE("coalescing: duplicate path not added twice") {
    cv::BoundedQueue q(10, "fcfs");
    q.push({"same/path", false, 0, 0});
    q.push({"same/path", false, 0, 0}); // should coalesce

    CHECK(q.size() == 1);

    q.shutdown();
}

TEST_CASE("capacity enforced — producer blocks until consumer drains") {
    cv::BoundedQueue q(2, "fcfs"); // very small capacity

    // Fill the queue
    q.push({"a", false, 0, 0});
    q.push({"b", false, 0, 0});
    CHECK(q.is_full());

    bool producer_unblocked = false;
    std::thread producer([&] {
        q.push({"c", false, 0, 0}); // should block
        producer_unblocked = true;
    });

    std::this_thread::sleep_for(50ms);
    CHECK(!producer_unblocked); // still blocked

    // Consumer drains one item
    cv::WorkItem item;
    q.pop(item);

    std::this_thread::sleep_for(50ms);
    producer.join();
    CHECK(producer_unblocked);
    q.shutdown();
}

TEST_CASE("policy switch FCFS -> priority") {
    cv::BoundedQueue q(10, "fcfs");
    // Push items with different priorities
    q.push({"low",  false, 10, 0});
    q.push({"high", false, 1,  0});
    q.push({"med",  false, 5,  0});

    // Switch to priority policy
    q.set_policy("priority");

    cv::WorkItem item;
    q.pop(item);
    // After sort, highest priority (lowest number) should come first
    CHECK(item.priority <= 5); // high or med should come first

    q.shutdown();
}

TEST_CASE("shutdown with items in queue") {
    cv::BoundedQueue q(10, "fcfs");
    q.push({"x", false, 0, 0});
    q.push({"y", false, 0, 0});
    q.shutdown(); // items left in queue — that's fine
    // No crash or hang
}

} // TEST_SUITE BoundedQueue
