// src/pipeline/buffer_pool.h — Bounded-memory read budget
//
// WHY: Worker threads read whole files before chunking them. Without a budget,
// N workers could each read a multi-gigabyte file and exhaust memory. The pool
// is a byte-counting semaphore: a worker acquires a lease for the file size,
// holds it through read + hash + chunk + manifest, then releases it.
//
// OVERSIZED FILES: a single file larger than the whole budget is allowed to run
// alone (the lease reserves the full budget). This avoids a deadlock while
// still guaranteeing that only one oversized reader is active at a time.
#pragma once
#include <cstddef>
#include <mutex>
#include <condition_variable>

namespace cv {

class BufferPool {
public:
    // Move-only RAII lease. Releasing is automatic on scope exit.
    class Lease {
    public:
        Lease() = default;
        Lease(BufferPool *pool, size_t bytes) : pool_(pool), bytes_(bytes) {}
        ~Lease() { release(); }

        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;

        Lease(Lease &&other) noexcept
            : pool_(other.pool_), bytes_(other.bytes_) {
            other.pool_ = nullptr;
            other.bytes_ = 0;
        }
        Lease &operator=(Lease &&other) noexcept {
            if (this != &other) {
                release();
                pool_ = other.pool_;
                bytes_ = other.bytes_;
                other.pool_ = nullptr;
                other.bytes_ = 0;
            }
            return *this;
        }

        void release() {
            if (pool_) {
                pool_->release_bytes(bytes_);
                pool_ = nullptr;
                bytes_ = 0;
            }
        }

        size_t bytes() const { return bytes_; }
        bool   valid() const { return pool_ != nullptr; }

    private:
        BufferPool *pool_ = nullptr;
        size_t      bytes_ = 0;
    };

    explicit BufferPool(size_t capacity_bytes);

    // Block until `bytes` fit in the budget (or until the pool is empty for an
    // oversized request). Returns a lease that must outlive the buffer.
    Lease acquire(size_t bytes);

    size_t capacity()  const;
    size_t in_use()    const;
    size_t available() const;

private:
    friend class Lease;
    void release_bytes(size_t bytes);

    const size_t            capacity_;
    mutable std::mutex      mu_;
    std::condition_variable cv_;
    size_t                  used_ = 0;
};

} // namespace cv
