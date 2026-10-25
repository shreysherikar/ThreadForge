#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace threadforge {

/**
 * @brief Manages a fixed-size pool of worker threads.
 *
 * Responsibilities:
 * - Thread lifecycle management (spawn, execution loop, join).
 * - Identification of worker threads to prevent deadlocks (e.g. self-joining on shutdown).
 * - Safe exception boundaries for worker threads.
 */
class WorkerPool {
public:
    /**
     * @brief Type of work loop executed by each worker thread.
     * Takes worker index as argument.
     */
    using WorkerRoutine = std::function<void(std::size_t worker_index)>;

    /**
     * @brief Constructs a worker pool with a specified number of threads.
     *
     * @param num_workers Number of threads to instantiate. Must be > 0.
     * @param routine The loop routine executed by each worker thread.
     * @throws std::invalid_argument If num_workers == 0.
     */
    WorkerPool(std::size_t num_workers, WorkerRoutine routine);

    /**
     * @brief Destructor. Ensures all worker threads are joined before destruction.
     */
    ~WorkerPool();

    // WorkerPool is non-copyable and non-movable
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;
    WorkerPool(WorkerPool&&) = delete;
    WorkerPool& operator=(WorkerPool&&) = delete;

    /**
     * @brief Returns the number of worker threads in the pool.
     */
    [[nodiscard]] std::size_t size() const noexcept { return num_workers_; }

    /**
     * @brief Determines whether the calling thread is one of the pool's worker threads.
     *
     * Used to detect and reject self-join attempts during graceful shutdown.
     */
    [[nodiscard]] bool is_current_thread_worker() const noexcept;

    /**
     * @brief Joins all worker threads. Safe to call multiple times (idempotent).
     */
    void join_all();

private:
    std::size_t num_workers_{0};
    WorkerRoutine routine_{nullptr};
    std::vector<std::thread> threads_{};
    std::vector<std::thread::id> thread_ids_{};
    mutable std::mutex pool_mutex_{};
    bool joined_{false};
    std::atomic<bool> start_flag_{false};
};

} // namespace threadforge
