// src/watcher/debouncer.h — Event debouncer
//
// WHY: Editors commonly generate a burst of inotify events for a single
// logical "save". For example, vim's atomic-rename approach:
//   1. Write to a temp file  → IN_CREATE + IN_CLOSE_WRITE (temp file)
//   2. Rename temp -> target → IN_MOVED_FROM (temp) + IN_MOVED_TO (target)
//   3. Set permissions       → IN_ATTRIB
//
// If we version after every event, we'd either version temp files or trigger
// multiple versions for one save.
//
// DEBOUNCE ALGORITHM: Track the last-event timestamp per path. Only emit a
// "settled" event when the path has been quiet for debounce_ms milliseconds.
// This coalesces all events for one save into a single notification.
//
// IMPLEMENTATION: A background thread sleeps on a condition variable until
// the nearest pending deadline (or until a new event arrives), then emits the
// paths that have been quiet for debounce_ms. The map is protected by a mutex;
// the thread sleeps most of the time, so contention is low.
#pragma once
#include <string>
#include <unordered_map>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <atomic>

namespace cv {

struct SettledEvent {
    std::string path;
    bool        deleted; // true if the last event was a DELETE
};

using SettledCallback = std::function<void(SettledEvent)>;

class Debouncer {
public:
    // debounce_ms: quiet window in milliseconds
    explicit Debouncer(int debounce_ms, SettledCallback cb);
    ~Debouncer();

    void start();
    void stop();

    // Push a raw event into the debouncer
    void push(const std::string &path, bool deleted = false);

    // For testing: get the count of paths currently pending
    size_t pending_count() const;

private:
    void scan_loop();

    struct Entry {
        std::chrono::steady_clock::time_point deadline;
        bool deleted;
    };

    int               debounce_ms_;
    SettledCallback   callback_;
    std::unordered_map<std::string, Entry> pending_;
    mutable std::mutex               mu_;
    std::condition_variable          cv_;
    std::thread                      thread_;
    std::atomic<bool>                running_{false};
};

} // namespace cv
