// src/watcher/debouncer.cc — Debouncer implementation
#include "debouncer.h"
#include "../daemon/logger.h"
#include <sstream>

namespace cv {

Debouncer::Debouncer(int debounce_ms, SettledCallback cb)
    : debounce_ms_(debounce_ms)
    , callback_(std::move(cb))
{}

Debouncer::~Debouncer() { stop(); }

void Debouncer::start() {
    running_.store(true);
    thread_ = std::thread(&Debouncer::scan_loop, this);
}

void Debouncer::stop() {
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void Debouncer::push(const std::string &path, bool deleted) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(debounce_ms_);
    {
        std::lock_guard<std::mutex> lk(mu_);
        // Update (or insert) the entry — reset the deadline on each event.
        // WHY: If events keep arriving for the same path, the quiet window
        // keeps resetting. This correctly handles a file being written to
        // repeatedly in rapid succession.
        auto &entry = pending_[path];
        entry.deadline = deadline;
        entry.deleted  = deleted;
    }
    // Wake the scan thread so it can recalculate its next sleep interval
    cv_.notify_one();
}

size_t Debouncer::pending_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return pending_.size();
}

void Debouncer::scan_loop() {
    while (running_.load()) {
        std::unique_lock<std::mutex> lk(mu_);

        // Find the nearest deadline (or sleep for debounce_ms if nothing pending)
        auto wake_at = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(debounce_ms_);
        for (auto &[path, entry] : pending_) {
            if (entry.deadline < wake_at) wake_at = entry.deadline;
        }

        // Wait until the nearest deadline or until push() wakes us
        // WHY condition_variable::wait_until: avoids spinning; thread sleeps
        // in the kernel (FUTEX_WAIT) until woken or the deadline passes.
        cv_.wait_until(lk, wake_at, [this]{ return !running_.load(); });

        if (!running_.load()) break;

        auto now = std::chrono::steady_clock::now();

        // Collect all paths whose quiet window has elapsed
        std::vector<SettledEvent> ready;
        for (auto it = pending_.begin(); it != pending_.end(); ) {
            if (it->second.deadline <= now) {
                ready.push_back({ it->first, it->second.deleted });
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
        lk.unlock();

        // Fire callbacks outside the lock to avoid priority inversion
        for (auto &ev : ready) {
            LOG_DEBUG("Settled: " + ev.path + (ev.deleted ? " [deleted]" : ""));
            callback_(ev);
        }
    }
}

} // namespace cv
