// src/pipeline/bounded_queue.cc — Bounded queue implementation
#include "bounded_queue.h"
#include "../daemon/logger.h"
#include <algorithm>
#include <chrono>

namespace cv {

static uint64_t now_ms_q() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

BoundedQueue::BoundedQueue(size_t capacity, std::string policy)
    : capacity_(capacity), policy_(std::move(policy)) {}

bool BoundedQueue::push(WorkItem item) {
    item.enqueue_time_ms = now_ms_q();

    std::unique_lock<std::mutex> lk(mutex_);

    // Check for coalescing: if this path is already in the queue,
    // just update its metadata (priority, timestamp) rather than adding a
    // duplicate. This is the backpressure coalescing policy.
    if (path_index_.count(item.path)) {
        // Update existing entry — find it and refresh its priority
        for (auto &wi : queue_) {
            if (wi.path == item.path) {
                wi.deleted  = item.deleted;
                wi.priority = std::min(wi.priority, item.priority); // higher prio wins
                break;
            }
        }
        // No change to queue size — no need to notify consumers
        return true;
    }

    // Wait for space if full (back-pressure)
    // WHY wait with predicate: guards against spurious wakeups
    not_full_.wait(lk, [this] {
        return queue_.size() < capacity_ || shutdown_.load();
    });

    if (shutdown_.load()) return false;

    path_index_[item.path] = queue_.size();
    queue_.push_back(std::move(item));

    maybe_sort();

    // Notify one waiting consumer
    not_empty_.notify_one();
    return true;
}

bool BoundedQueue::pop(WorkItem &out) {
    std::unique_lock<std::mutex> lk(mutex_);

    // Wait for items (or shutdown)
    not_empty_.wait(lk, [this] {
        return !queue_.empty() || shutdown_.load();
    });

    if (queue_.empty()) return false; // shutdown

    // Apply aging for Priority policy: items waiting longer get boosted
    // WHY aging: prevents starvation of large low-priority files when small
    // files keep arriving. After AGING_THRESHOLD_MS milliseconds, priority
    // is set to 0 (highest) regardless of original priority.
    if (policy_ == "priority") {
        constexpr uint64_t AGING_THRESHOLD_MS = 5000;
        uint64_t now = now_ms_q();
        for (auto &wi : queue_) {
            if (now - wi.enqueue_time_ms > AGING_THRESHOLD_MS) {
                wi.priority = 0; // promote to highest priority
            }
        }
        // Re-sort after aging
        std::stable_sort(queue_.begin(), queue_.end(),
            [](const WorkItem &a, const WorkItem &b) {
                return a.priority < b.priority;
            });
    }

    out = std::move(queue_.front());
    queue_.pop_front();
    path_index_.erase(out.path);

    // Notify producers that there's space
    not_full_.notify_one();
    return true;
}

void BoundedQueue::shutdown() {
    shutdown_.store(true);
    not_full_.notify_all();
    not_empty_.notify_all();
}

void BoundedQueue::set_policy(const std::string &policy) {
    std::lock_guard<std::mutex> lk(mutex_);
    policy_ = policy;
    maybe_sort();
}

void BoundedQueue::maybe_sort() {
    if (policy_ == "priority") {
        std::stable_sort(queue_.begin(), queue_.end(),
            [](const WorkItem &a, const WorkItem &b) {
                return a.priority < b.priority;
            });
        // Rebuild path_index_ after sort
        path_index_.clear();
        for (size_t i = 0; i < queue_.size(); i++) {
            path_index_[queue_[i].path] = i;
        }
    }
}

size_t BoundedQueue::size() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return queue_.size();
}

bool BoundedQueue::is_full() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return queue_.size() >= capacity_;
}

bool BoundedQueue::is_empty() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return queue_.empty();
}

} // namespace cv
