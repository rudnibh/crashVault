// src/pipeline/buffer_pool.cc — Bounded-memory read budget implementation
#include "buffer_pool.h"
#include <algorithm>

namespace cv {

BufferPool::BufferPool(size_t capacity_bytes)
    : capacity_(std::max<size_t>(capacity_bytes, 1)) {}

BufferPool::Lease BufferPool::acquire(size_t bytes) {
    if (bytes == 0) return Lease();

    std::unique_lock<std::mutex> lk(mu_);

    if (bytes > capacity_) {
        // Oversized file: wait for the pool to drain, then reserve the whole
        // budget. Only one oversized reader can be active at a time.
        cv_.wait(lk, [this] { return used_ == 0; });
        used_ = capacity_;
        return Lease(this, capacity_);
    }

    cv_.wait(lk, [this, bytes] { return used_ + bytes <= capacity_; });
    used_ += bytes;
    return Lease(this, bytes);
}

void BufferPool::release_bytes(size_t bytes) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        used_ = (used_ > bytes) ? used_ - bytes : 0;
    }
    cv_.notify_all();
}

size_t BufferPool::capacity() const { return capacity_; }

size_t BufferPool::in_use() const {
    std::lock_guard<std::mutex> lk(mu_);
    return used_;
}

size_t BufferPool::available() const {
    std::lock_guard<std::mutex> lk(mu_);
    return capacity_ - used_;
}

} // namespace cv
