#include <threadforge/worker_pool.hpp>

#include <algorithm>
#include <utility>

namespace threadforge {

WorkerPool::WorkerPool(std::size_t num_workers, WorkerRoutine routine)
    : num_workers_(num_workers), routine_(std::move(routine)) {
    if (num_workers_ == 0) {
        throw std::invalid_argument("Worker count must be greater than zero");
    }

    threads_.reserve(num_workers_);
    thread_ids_.reserve(num_workers_);

    for (std::size_t i = 0; i < num_workers_; ++i) {
        threads_.emplace_back([this, i]() {
            // Spin-wait briefly until all thread IDs have been recorded by the constructor
            while (!start_flag_.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            if (routine_) {
                routine_(i);
            }
        });
        thread_ids_.push_back(threads_.back().get_id());
    }

    start_flag_.store(true, std::memory_order_release);
}

WorkerPool::~WorkerPool() {
    join_all();
}

bool WorkerPool::is_current_thread_worker() const noexcept {
    const auto current_id = std::this_thread::get_id();
    // thread_ids_ is immutable after constructor completes; safe to read concurrently without locks
    for (const auto& id : thread_ids_) {
        if (id == current_id) {
            return true;
        }
    }
    return false;
}

void WorkerPool::join_all() {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    if (joined_) {
        return;
    }
    for (auto& t : threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
    joined_ = true;
}

} // namespace threadforge
