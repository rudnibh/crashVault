// src/pipeline/worker_pool.cc — Worker pool implementation
#include "worker_pool.h"
#include "../daemon/logger.h"
#include <sys/resource.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <cerrno>
#include <cstring>
#include <sstream>

namespace cv {

WorkerPool::WorkerPool(int num_workers, BoundedQueue &queue, ProcessFn fn, int nice_level)
    : num_workers_(num_workers)
    , queue_(queue)
    , process_fn_(std::move(fn))
    , nice_level_(nice_level)
{}

WorkerPool::~WorkerPool() { stop(); }

void WorkerPool::start() {
    for (int i = 0; i < num_workers_; i++) {
        threads_.emplace_back(&WorkerPool::worker_main, this, i);
    }
}

void WorkerPool::stop() {
    stopping_.store(true);
    queue_.shutdown();
    for (auto &t : threads_) {
        if (t.joinable()) t.join();
    }
    threads_.clear();
}

void WorkerPool::worker_main(int id) {
    // WHY setpriority: Lower our scheduling priority (nice +10) so that
    // the versioning work doesn't compete with foreground applications.
    // PRIO_PROCESS with tid=0 sets the calling thread's priority.
    // Note: on Linux, setpriority applies to the calling LWP (lightweight
    // process / kernel thread), so each thread can have its own nice level.
    pid_t tid = (pid_t)syscall(SYS_gettid);
    if (setpriority(PRIO_PROCESS, (id_t)tid, nice_level_) != 0) {
        LOG_WARN("setpriority failed for worker " + std::to_string(id) +
                 ": " + strerror(errno));
    }

    LOG_INFO("Worker " + std::to_string(id) + " started (tid=" +
             std::to_string(tid) + ", nice=" + std::to_string(nice_level_) + ")");

    WorkItem item;
    while (!stopping_.load()) {
        if (!queue_.pop(item)) break; // queue shut down

        active_.fetch_add(1);
        try {
            process_fn_(item);
        } catch (const std::exception &e) {
            LOG_ERROR("Worker " + std::to_string(id) + " exception: " + e.what());
        } catch (...) {
            LOG_ERROR("Worker " + std::to_string(id) + " unknown exception");
        }
        active_.fetch_sub(1);
    }

    LOG_INFO("Worker " + std::to_string(id) + " exiting");
}

} // namespace cv
