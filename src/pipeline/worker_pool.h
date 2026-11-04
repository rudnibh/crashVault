// src/pipeline/worker_pool.h — Worker thread pool
//
// WHY: Processing file versions (reading, hashing, storing) is CPU and I/O
// intensive and should not block the inotify event loop. Worker threads run
// at reduced scheduling priority (nice +10) so the daemon is a "good citizen"
// and doesn't starve foreground applications.
//
// WHY nice/setpriority: Linux's completely fair scheduler (CFS) gives the
// daemon a lower weight, reducing its impact on interactive workloads.
// This demonstrates the SCHEDULING POLICY OS concept.
#pragma once
#include <vector>
#include <thread>
#include <functional>
#include <atomic>
#include <memory>
#include "bounded_queue.h"

namespace cv {

// Forward decls
class Config;

// ProcessFn processes one work item
using ProcessFn = std::function<void(const WorkItem &)>;

class WorkerPool {
public:
    WorkerPool(int num_workers, BoundedQueue &queue, ProcessFn fn, int nice_level = 10);
    ~WorkerPool();

    void start();
    void stop(); // drain and join

    int active_workers() const { return active_.load(); }

private:
    void worker_main(int id);

    int                     num_workers_;
    BoundedQueue           &queue_;
    ProcessFn               process_fn_;
    int                     nice_level_;
    std::vector<std::thread> threads_;
    std::atomic<int>        active_{0};
    std::atomic<bool>       stopping_{false};
};

} // namespace cv
