// src/daemon/config.h — Configuration loader
// WHY: A config file lets the user customise watched roots, ignore patterns,
// worker count, memory cap, debounce window, and scheduling policy without
// recompiling. Parsed once at startup; reloaded on SIGHUP.
#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace cv {

struct Config {
    // Directories to watch (absolute paths)
    std::vector<std::string> watched_roots;

    // Glob-style patterns for files/dirs to ignore
    // Default: *.swp, *~, .git/, .crashvault/
    std::vector<std::string> ignore_patterns;

    // Number of worker threads in the pipeline
    int worker_count = 4;

    // Maximum memory the daemon may use for chunk buffers (bytes)
    size_t memory_cap_bytes = 256ULL * 1024 * 1024; // 256 MiB

    // Debounce quiet window: how long a path must be quiescent before
    // it is handed to the pipeline (milliseconds)
    int debounce_ms = 300;

    // Queue scheduling policy: "fcfs" or "priority"
    std::string scheduling_policy = "fcfs";

    // Retention: keep this many versions per file
    int retention_versions = 10;

    // Retention: keep daily snapshots for this many days
    int retention_days = 30;

    // Store root (defaults to ~/.crashvault)
    std::string store_root;

    // mmap threshold: files larger than this are mmapped instead of read()
    size_t mmap_threshold_bytes = 1ULL * 1024 * 1024; // 1 MiB

    // Quiescence window for auto-snapshots (seconds)
    int snapshot_quiescence_s = 5;

    // Web gateway port
    int gateway_port = 8080;

    // Load from an INI file; returns false and fills error on parse failure
    bool load(const std::string &path, std::string &error);

    // Reload (called on SIGHUP). The caller must hold the daemon config
    // mutex because IPC threads may be reading the same Config object.
    bool reload(const std::string &path, std::string &error);

    // Print a summary to stderr (for --foreground debugging)
    void dump() const;
};

} // namespace cv
