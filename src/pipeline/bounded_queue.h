// src/pipeline/bounded_queue.h — Thread-safe bounded producer-consumer queue
//
// WHY: The watcher/debouncer (producer) generates work items faster than the
// worker pool (consumer) might process them, especially during burst writes.
// A BOUNDED queue provides back-pressure: the producer blocks (or coalesces)
// when the queue is full, preventing unbounded memory growth.
//
// IMPLEMENTATION: Classic two-condition-variable monitor pattern:
//   - mutex: protects the deque
//   - not_full:  producer waits here when queue is at capacity
//   - not_empty: consumer waits here when queue is empty
//
// WHY NOT std::condition_variable::wait with pred: We use the predicate form
// to guard against spurious wakeups (which POSIX allows for condvars backed
// by futexes — the kernel may wake waiters for internal reasons).
//
// DEADLOCK FREEDOM: There is exactly ONE lock (mutex_). Both CVs are
// associated with this same lock. Lock ordering is trivially satisfied.
// Starvation: when the queue is full, the producer either waits (FCFS) or
// coalesces duplicate paths (priority policy), so the queue never grows
// without bound.
#pragma once
#include <deque>
#include <string>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <unordered_map>

namespace cv {

struct WorkItem {
    std::string path;
    bool        deleted;
    int         priority; // lower = higher priority (for Priority policy)
    uint64_t    enqueue_time_ms; // for aging anti-starvation
};

class BoundedQueue {
public:
    // capacity: maximum number of items; policy: "fcfs" or "priority"
    explicit BoundedQueue(size_t capacity, std::string policy = "fcfs");

    // Push item — blocks if full (unless coalescing is possible).
    // Returns false only if the queue is shut down.
    bool push(WorkItem item);

    // Pop item — blocks until an item is available or the queue is shut down.
    // Returns false if the queue is shut down and empty.
    bool pop(WorkItem &out);

    // Signal all waiters to unblock and exit — called at shutdown.
    void shutdown();

    // Runtime policy switch (called via IPC)
    void set_policy(const std::string &policy);

    size_t size() const;
    bool   is_full() const;
    bool   is_empty() const;
    size_t capacity() const { return capacity_; }

private:
    // Reorder deque according to current policy (called after push)
    void maybe_sort();

    size_t                  capacity_;
    std::string             policy_;
    std::deque<WorkItem>    queue_;
    // For coalescing: track paths already in the queue
    std::unordered_map<std::string, size_t> path_index_;

    mutable std::mutex      mutex_;
    std::condition_variable not_full_;    // producer waits here
    std::condition_variable not_empty_;   // consumer waits here
    std::atomic<bool>       shutdown_{false};
};

} // namespace cv
