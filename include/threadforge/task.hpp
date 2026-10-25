#pragma once

#include <threadforge/task_priority.hpp>
#include <threadforge/task_status.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace threadforge {

/// Unique identifier for each submitted task.
using TaskId = uint64_t;

/// Sentinel value representing an unassigned or invalid TaskId.
inline constexpr TaskId kInvalidTaskId = 0;

/**
 * @brief Exception thrown when a caller attempts to retrieve the result of a task
 * that was cancelled prior to starting execution (e.g. via non-draining shutdown).
 */
class TaskCancelledException : public std::runtime_error {
public:
    TaskCancelledException()
        : std::runtime_error("ThreadForge: Task was cancelled prior to execution") {}
};

/**
 * @brief Thread-safe shared internal record tracking execution metadata for a task.
 *
 * Owned jointly by the scheduler's historical registry and the scheduled task closure.
 */
struct TaskRecord {
    TaskId id{kInvalidTaskId};
    std::string name;
    TaskPriority priority{TaskPriority::Normal};
    uint64_t sequence_number{0};

    std::chrono::system_clock::time_point submission_time{};
    std::chrono::system_clock::time_point start_time{};
    std::chrono::system_clock::time_point finish_time{};

    std::atomic<TaskStatus> status{TaskStatus::Pending};
    std::exception_ptr exception{nullptr};

    mutable std::mutex wait_mutex;
    mutable std::condition_variable wait_cv;

    TaskRecord() = default;

    TaskRecord(TaskId task_id, std::string task_name, TaskPriority prio, uint64_t seq)
        : id(task_id),
          name(std::move(task_name)),
          priority(prio),
          sequence_number(seq),
          submission_time(std::chrono::system_clock::now()) {}

    // Non-copyable, non-movable due to mutex and condition_variable
    TaskRecord(const TaskRecord&) = delete;
    TaskRecord& operator=(const TaskRecord&) = delete;
    TaskRecord(TaskRecord&&) = delete;
    TaskRecord& operator=(TaskRecord&&) = delete;

    [[nodiscard]] bool is_terminal() const noexcept {
        return threadforge::is_terminal(status.load(std::memory_order_acquire));
    }
};

/**
 * @brief Immutable snapshot of task metadata for public inspection.
 */
struct TaskMetadata {
    TaskId id{kInvalidTaskId};
    std::string name;
    TaskPriority priority{TaskPriority::Normal};
    uint64_t sequence_number{0};
    std::chrono::system_clock::time_point submission_time{};
    std::chrono::system_clock::time_point start_time{};
    std::chrono::system_clock::time_point finish_time{};
    TaskStatus status{TaskStatus::Pending};
    bool has_exception{false};
};

/**
 * @brief Executable task container held in the scheduler's priority queue.
 */
class Task {
public:
    Task() = default;

    Task(std::shared_ptr<TaskRecord> record,
         std::function<void()> work,
         std::function<void()> cancel_handler = nullptr)
        : record_(std::move(record)),
          work_(std::move(work)),
          cancel_handler_(std::move(cancel_handler)) {}

    [[nodiscard]] TaskId id() const noexcept {
        return record_ ? record_->id : kInvalidTaskId;
    }

    [[nodiscard]] const std::string& name() const noexcept {
        static const std::string kEmpty;
        return record_ ? record_->name : kEmpty;
    }

    [[nodiscard]] TaskPriority priority() const noexcept {
        return record_ ? record_->priority : TaskPriority::Normal;
    }

    [[nodiscard]] uint64_t sequence_number() const noexcept {
        return record_ ? record_->sequence_number : 0;
    }

    [[nodiscard]] const std::shared_ptr<TaskRecord>& record() const noexcept {
        return record_;
    }

    /// Invokes the wrapped task callable.
    void execute() const {
        if (work_) {
            work_();
        }
    }

    /// Cancels the task if aborted before execution.
    void cancel() const {
        if (cancel_handler_) {
            cancel_handler_();
        }
    }

    explicit operator bool() const noexcept {
        return record_ != nullptr && static_cast<bool>(work_);
    }

private:
    std::shared_ptr<TaskRecord> record_{nullptr};
    std::function<void()> work_{nullptr};
    std::function<void()> cancel_handler_{nullptr};
};

/**
 * @brief Custom comparator for std::priority_queue.
 *
 * Implements strict weak ordering for tasks:
 * 1. Primary ordering: TaskPriority descending. Higher priority tasks come first.
 * 2. Secondary ordering: Sequence number ascending. Equal-priority tasks follow FIFO order.
 *
 * Note on std::priority_queue semantics:
 * In C++, std::priority_queue is a max-heap where the "top" element is the one
 * that Compare(top, other) returns false for.
 * Thus:
 * - If lhs has lower priority than rhs, lhs < rhs is true (rhs comes first).
 * - If lhs has the same priority but a higher sequence number (submitted later),
 *   lhs < rhs is true (rhs was submitted earlier and comes first).
 */
struct TaskComparator {
    bool operator()(const Task& lhs, const Task& rhs) const noexcept {
        if (lhs.priority() != rhs.priority()) {
            return lhs.priority() < rhs.priority();
        }
        // Strict FIFO for equal priority: smaller sequence number has higher precedence
        return lhs.sequence_number() > rhs.sequence_number();
    }
};

/**
 * @brief Handle representing the future result and status of an asynchronously scheduled task.
 *
 * @tparam T Return type of the scheduled task.
 */
template <typename T>
class TaskFuture {
public:
    TaskFuture() = default;

    TaskFuture(TaskId id, std::future<T> future, std::shared_ptr<TaskRecord> record)
        : id_(id), future_(std::move(future)), record_(std::move(record)) {}

    TaskFuture(const TaskFuture&) = delete;
    TaskFuture& operator=(const TaskFuture&) = delete;
    TaskFuture(TaskFuture&&) noexcept = default;
    TaskFuture& operator=(TaskFuture&&) noexcept = default;

    [[nodiscard]] TaskId id() const noexcept { return id_; }

    [[nodiscard]] bool valid() const noexcept { return future_.valid(); }

    [[nodiscard]] TaskStatus status() const noexcept {
        if (!record_) {
            return TaskStatus::Failed;
        }
        return record_->status.load(std::memory_order_acquire);
    }

    void wait() const {
        ensure_valid();
        future_.wait();
    }

    template <typename Rep, typename Period>
    std::future_status wait_for(const std::chrono::duration<Rep, Period>& timeout_duration) const {
        ensure_valid();
        return future_.wait_for(timeout_duration);
    }

    template <typename Clock, typename Duration>
    std::future_status wait_until(const std::chrono::time_point<Clock, Duration>& timeout_time) const {
        ensure_valid();
        return future_.wait_until(timeout_time);
    }

    /**
     * @brief Retrieves the result of the task.
     *
     * Blocks if the task is still executing.
     * If the task threw an exception or was cancelled, get() rethrows that exception.
     *
     * @throws std::runtime_error If the result has already been retrieved or the future is invalid.
     * @throws TaskCancelledException If the task was cancelled before execution.
     */
    T get() {
        ensure_valid();
        return future_.get();
    }

private:
    void ensure_valid() const {
        if (!future_.valid()) {
            throw std::runtime_error("TaskFuture: Result has already been retrieved or handle is invalid");
        }
    }

    TaskId id_{kInvalidTaskId};
    std::future<T> future_{};
    std::shared_ptr<TaskRecord> record_{nullptr};
};

/**
 * @brief Specialization of TaskFuture for void-returning tasks.
 */
template <>
class TaskFuture<void> {
public:
    TaskFuture() = default;

    TaskFuture(TaskId id, std::future<void> future, std::shared_ptr<TaskRecord> record)
        : id_(id), future_(std::move(future)), record_(std::move(record)) {}

    TaskFuture(const TaskFuture&) = delete;
    TaskFuture& operator=(const TaskFuture&) = delete;
    TaskFuture(TaskFuture&&) noexcept = default;
    TaskFuture& operator=(TaskFuture&&) noexcept = default;

    [[nodiscard]] TaskId id() const noexcept { return id_; }

    [[nodiscard]] bool valid() const noexcept { return future_.valid(); }

    [[nodiscard]] TaskStatus status() const noexcept {
        if (!record_) {
            return TaskStatus::Failed;
        }
        return record_->status.load(std::memory_order_acquire);
    }

    void wait() const {
        ensure_valid();
        future_.wait();
    }

    template <typename Rep, typename Period>
    std::future_status wait_for(const std::chrono::duration<Rep, Period>& timeout_duration) const {
        ensure_valid();
        return future_.wait_for(timeout_duration);
    }

    template <typename Clock, typename Duration>
    std::future_status wait_until(const std::chrono::time_point<Clock, Duration>& timeout_time) const {
        ensure_valid();
        return future_.wait_until(timeout_time);
    }

    /**
     * @brief Waits for completion and rethrows any exception if the task failed or was cancelled.
     *
     * @throws std::runtime_error If already retrieved or future is invalid.
     * @throws TaskCancelledException If the task was cancelled before execution.
     */
    void get() {
        ensure_valid();
        future_.get();
    }

private:
    void ensure_valid() const {
        if (!future_.valid()) {
            throw std::runtime_error("TaskFuture: Result has already been retrieved or handle is invalid");
        }
    }

    TaskId id_{kInvalidTaskId};
    std::future<void> future_{};
    std::shared_ptr<TaskRecord> record_{nullptr};
};

} // namespace threadforge
