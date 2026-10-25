#pragma once

#include <threadforge/scheduler_stats.hpp>
#include <threadforge/task.hpp>
#include <threadforge/task_priority.hpp>
#include <threadforge/task_status.hpp>
#include <threadforge/worker_pool.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace threadforge {

/**
 * @brief Exception thrown when task submission is attempted on a scheduler
 * that is currently shutting down or has already completed shutdown.
 */
class SchedulerShutdownException : public std::runtime_error {
public:
    SchedulerShutdownException()
        : std::runtime_error("ThreadForge: Scheduler has shut down; new tasks are rejected") {}
};

/**
 * @brief Production-quality multithreaded priority task scheduler.
 *
 * Key Architectural Characteristics:
 * - Deterministic Priority Scheduling with FIFO tie-breaking for equal priority tasks.
 * - Thread-safe concurrent multi-producer submissions.
 * - Fixed-size worker pool with zero busy-waiting.
 * - Exception isolation: task exceptions are captured and stored without crashing workers.
 * - Graceful shutdown contract: drains pending tasks, preserves invariant, detects worker self-joins.
 * - Comprehensive aggregate statistics and bounded historical record retention.
 */
class TaskScheduler {
public:
    /**
     * @brief Constructs a TaskScheduler.
     *
     * @param num_workers Number of worker threads. Defaults to hardware concurrency (min 2).
     * @param max_history_records Maximum historical task records to maintain before pruning.
     *                            0 indicates unbounded retention until manual clear.
     * @throws std::invalid_argument If num_workers == 0.
     */
    explicit TaskScheduler(
        std::size_t num_workers = default_worker_count(),
        std::size_t max_history_records = 10000);

    /**
     * @brief Destructor. Automatically triggers graceful shutdown and joins all workers.
     */
    ~TaskScheduler();

    // TaskScheduler is non-copyable and non-movable
    TaskScheduler(const TaskScheduler&) = delete;
    TaskScheduler& operator=(const TaskScheduler&) = delete;
    TaskScheduler(TaskScheduler&&) = delete;
    TaskScheduler& operator=(TaskScheduler&&) = delete;

    /**
     * @brief Submits a callable task with default (Normal) priority.
     */
    template <typename F, typename... Args>
    auto submit(F&& f, Args&&... args) {
        return submit_named("", TaskPriority::Normal, std::forward<F>(f), std::forward<Args>(args)...);
    }

    /**
     * @brief Submits a callable task with an explicit scheduling priority.
     */
    template <typename F, typename... Args>
    auto submit_with_priority(TaskPriority priority, F&& f, Args&&... args) {
        return submit_named("", priority, std::forward<F>(f), std::forward<Args>(args)...);
    }

    /**
     * @brief Submits a named callable task with an explicit scheduling priority.
     *
     * @param name Descriptive human-readable label for the task.
     * @param priority Priority tier for execution ordering.
     * @param f Callable object.
     * @param args Arguments forwarded to the callable.
     * @return TaskFuture<R> Handle to retrieve the result, wait, and inspect task status.
     * @throws SchedulerShutdownException If the scheduler is shutting down or has shut down.
     */
    template <typename F, typename... Args>
    auto submit_named(std::string name, TaskPriority priority, F&& f, Args&&... args) {
        using ReturnType = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;

        // Bind callable with arguments into a parameterless callable
        auto bound_task = [func = std::forward<F>(f),
                           args_tuple = std::make_tuple(std::forward<Args>(args)...)]() mutable {
            return std::apply(std::move(func), std::move(args_tuple));
        };

        auto task_promise = std::make_shared<std::promise<ReturnType>>();
        auto future = task_promise->get_future();

        const TaskId id = next_task_id_.fetch_add(1, std::memory_order_relaxed);
        const uint64_t seq = sequence_counter_.fetch_add(1, std::memory_order_relaxed);

        auto record = std::make_shared<TaskRecord>(id, std::move(name), priority, seq);

        // Wrap execution in a void() callable that sets promise value/exception
        auto execution_wrapper = [record, task_promise, task_callable = std::move(bound_task)]() mutable {
            try {
                if constexpr (std::is_void_v<ReturnType>) {
                    task_callable();
                    const auto finish_sys = std::chrono::system_clock::now();
                    {
                        std::lock_guard<std::mutex> lock(record->wait_mutex);
                        record->finish_time = finish_sys;
                        record->status.store(TaskStatus::Completed, std::memory_order_release);
                        record->wait_cv.notify_all();
                    }
                    task_promise->set_value();
                } else {
                    auto result = task_callable();
                    const auto finish_sys = std::chrono::system_clock::now();
                    {
                        std::lock_guard<std::mutex> lock(record->wait_mutex);
                        record->finish_time = finish_sys;
                        record->status.store(TaskStatus::Completed, std::memory_order_release);
                        record->wait_cv.notify_all();
                    }
                    task_promise->set_value(std::move(result));
                }
            } catch (...) {
                auto exc = std::current_exception();
                const auto finish_sys = std::chrono::system_clock::now();
                {
                    std::lock_guard<std::mutex> lock(record->wait_mutex);
                    record->finish_time = finish_sys;
                    record->exception = exc;
                    record->status.store(TaskStatus::Failed, std::memory_order_release);
                    record->wait_cv.notify_all();
                }
                task_promise->set_exception(exc);
                // Rethrow so the scheduler's worker can capture exception for stats accounting
                throw;
            }
        };

        // Wrap cancellation in a void() callable for non-draining shutdown
        auto cancel_wrapper = [record, task_promise]() {
            auto exc = std::make_exception_ptr(TaskCancelledException());
            {
                std::lock_guard<std::mutex> lock(record->wait_mutex);
                record->exception = exc;
                record->finish_time = std::chrono::system_clock::now();
                record->status.store(TaskStatus::Cancelled, std::memory_order_release);
                record->wait_cv.notify_all();
            }
            task_promise->set_exception(exc);
        };

        Task task(record, std::move(execution_wrapper), std::move(cancel_wrapper));

        enqueue_task(std::move(task), record);

        return TaskFuture<ReturnType>(id, std::move(future), std::move(record));
    }

    /**
     * @brief Queries the current lifecycle status of a task by its ID.
     *
     * @param id The TaskId to query.
     * @return std::optional<TaskStatus> Status if task ID exists, std::nullopt if unknown.
     */
    [[nodiscard]] std::optional<TaskStatus> get_task_status(TaskId id) const;

    /**
     * @brief Waits for a specific task to reach a terminal state (Completed or Failed).
     *
     * @param id The TaskId to wait on.
     * @param timeout Maximum duration to wait. Defaults to indefinite.
     * @return true if the task reached a terminal state within timeout, false otherwise.
     * @throws std::invalid_argument If the TaskId is not found.
     */
    bool wait_for_task(
        TaskId id,
        std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) const;

    /**
     * @brief Retrieves a complete metadata snapshot for a task.
     *
     * @param id The TaskId to inspect.
     * @return std::optional<TaskMetadata> Snapshot if found, std::nullopt otherwise.
     */
    [[nodiscard]] std::optional<TaskMetadata> get_task_metadata(TaskId id) const;

    /**
     * @brief Retrieves the exception pointer if the task failed with an unhandled exception.
     *
     * @param id The TaskId to inspect.
     * @return std::exception_ptr Stored exception, or nullptr if completed successfully.
     * @throws std::invalid_argument If task ID is unknown.
     * @throws std::runtime_error If the task is still Pending or Running.
     */
    [[nodiscard]] std::exception_ptr get_task_exception(TaskId id) const;

    /**
     * @brief Rethrows the exception of a failed task.
     *
     * @param id The TaskId to inspect.
     * @throws std::invalid_argument If task ID is unknown.
     * @throws std::runtime_error If the task is still Pending or Running.
     */
    void rethrow_task_exception(TaskId id) const;

    /**
     * @brief Returns a consistent aggregate statistics snapshot.
     */
    [[nodiscard]] SchedulerStats get_stats() const;

    /**
     * @brief Checks whether the scheduler is currently accepting new task submissions.
     */
    [[nodiscard]] bool is_accepting_tasks() const noexcept {
        return accepting_tasks_.load(std::memory_order_acquire);
    }

    /**
     * @brief Returns true if all queues are empty and no worker is actively running a task.
     */
    [[nodiscard]] bool is_idle() const;

    /**
     * @brief Blocks until all currently queued and executing tasks have completed.
     */
    void wait_idle();

    /**
     * @brief Returns the total number of worker threads.
     */
    [[nodiscard]] std::size_t worker_count() const noexcept {
        return worker_pool_ ? worker_pool_->size() : 0;
    }

    /**
     * @brief Returns the number of tasks currently waiting in the priority queue.
     */
    [[nodiscard]] std::size_t queue_size() const;

    /**
     * @brief Shuts down the scheduler.
     *
     * Semantics:
     * 1. Atomically rejects all subsequent task submissions.
     * 2. If drain == true: All already-accepted tasks in the priority queue are drained
     *    and completed before workers stop.
     *    If drain == false: Pending tasks are cleared from the queue immediately.
     * 3. Running tasks are allowed to complete naturally.
     * 4. All worker threads are notified and safely joined.
     * 5. Safe for repeated calls (idempotent).
     *
     * @param drain Whether to process all remaining queued tasks before stopping.
     * @throws std::logic_error If called from within a worker thread (self-join deadlock prevention).
     */
    void shutdown(bool drain = true);

    /**
     * @brief Clears historical records for all completed or failed tasks.
     * Active (Pending / Running) tasks are strictly preserved.
     *
     * @return std::size_t Number of cleared historical task records.
     */
    std::size_t clear_history();

    /**
     * @brief Prunes terminal historical records exceeding retain_count.
     *
     * @param retain_count Maximum number of terminal records to retain.
     * @return std::size_t Number of pruned records.
     */
    std::size_t prune_history(std::size_t retain_count);

private:
    static std::size_t default_worker_count() noexcept;

    void worker_routine(std::size_t worker_index);
    void execute_task(const Task& task);
    void enqueue_task(Task task, std::shared_ptr<TaskRecord> record);
    std::shared_ptr<TaskRecord> find_record(TaskId id) const;
    std::size_t prune_history_unlocked(std::size_t retain_count);

    std::size_t max_history_records_{10000};

    std::atomic<bool> accepting_tasks_{true};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> drain_queued_{true};

    std::atomic<TaskId> next_task_id_{1};
    std::atomic<uint64_t> sequence_counter_{0};

    // Priority queue protected by queue_mutex_
    mutable std::mutex queue_mutex_{};
    std::condition_variable queue_cv_{};
    std::condition_variable drain_cv_{};
    std::priority_queue<Task, std::vector<Task>, TaskComparator> queue_{};

    // Task records registry protected by records_mutex_
    mutable std::mutex records_mutex_{};
    std::unordered_map<TaskId, std::shared_ptr<TaskRecord>> task_records_{};

    // Aggregate statistics
    mutable std::mutex stats_mutex_{};
    std::atomic<uint64_t> total_accepted_tasks_{0};
    std::atomic<uint64_t> pending_tasks_{0};
    std::atomic<uint64_t> running_tasks_{0};
    std::atomic<uint64_t> completed_tasks_{0};
    std::atomic<uint64_t> failed_tasks_{0};
    std::atomic<uint64_t> cancelled_tasks_{0};
    std::atomic<uint64_t> rejected_submissions_{0};
    std::atomic<uint64_t> total_execution_time_ns_{0};

    // Mutex serializing shutdown invocations
    mutable std::mutex shutdown_mutex_{};
    bool is_shutdown_{false};

    std::unique_ptr<WorkerPool> worker_pool_{nullptr};
};

} // namespace threadforge
