#include <threadforge/task_scheduler.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

namespace threadforge {

std::size_t TaskScheduler::default_worker_count() noexcept {
    const auto hw = std::thread::hardware_concurrency();
    return hw == 0 ? 4 : static_cast<std::size_t>(hw);
}

TaskScheduler::TaskScheduler(std::size_t num_workers, std::size_t max_history_records)
    : max_history_records_(max_history_records) {
    if (num_workers == 0) {
        throw std::invalid_argument("Worker count must be greater than zero");
    }

    worker_pool_ = std::make_unique<WorkerPool>(
        num_workers,
        [this](std::size_t worker_index) {
            worker_routine(worker_index);
        });
}

TaskScheduler::~TaskScheduler() {
    try {
        shutdown(true);
    } catch (...) {
        // Prevent exceptions from escaping destructor
    }
}

void TaskScheduler::enqueue_task(Task task, std::shared_ptr<TaskRecord> record) {
    // 1. Fast-path check: do not register if already shutting down
    if (!accepting_tasks_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> queue_lock(queue_mutex_);
        rejected_submissions_.fetch_add(1, std::memory_order_relaxed);
        throw SchedulerShutdownException();
    }

    // 2. Insert record into task_records_ FIRST so get_task_status(id) always finds it
    {
        std::lock_guard<std::mutex> records_lock(records_mutex_);
        task_records_[record->id] = record;
        if (max_history_records_ > 0 && task_records_.size() > max_history_records_ * 2) {
            prune_history_unlocked(max_history_records_);
        }
    }

    // 3. Lock queue_mutex_, re-check accepting_tasks_, and push task to queue_
    try {
        std::lock_guard<std::mutex> queue_lock(queue_mutex_);
        if (!accepting_tasks_.load(std::memory_order_acquire)) {
            rejected_submissions_.fetch_add(1, std::memory_order_relaxed);
            throw SchedulerShutdownException();
        }

        queue_.push(std::move(task));
        pending_tasks_.fetch_add(1, std::memory_order_relaxed);
        total_accepted_tasks_.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
        // Rollback insertion into task_records_ on rejection or queue push failure
        {
            std::lock_guard<std::mutex> records_lock(records_mutex_);
            task_records_.erase(record->id);
        }
        throw;
    }

    queue_cv_.notify_one();
}

std::shared_ptr<TaskRecord> TaskScheduler::find_record(TaskId id) const {
    std::lock_guard<std::mutex> lock(records_mutex_);
    auto it = task_records_.find(id);
    if (it != task_records_.end()) {
        return it->second;
    }
    return nullptr;
}

void TaskScheduler::worker_routine(std::size_t /*worker_index*/) {
    while (true) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this]() {
                if (!queue_.empty()) {
                    return true;
                }
                if (stop_requested_.load(std::memory_order_acquire)) {
                    if (!drain_queued_.load(std::memory_order_acquire) || queue_.empty()) {
                        return true;
                    }
                }
                return false;
            });

            // Termination condition: queue empty and stop requested
            if (queue_.empty() && stop_requested_.load(std::memory_order_acquire)) {
                break;
            }

            task = queue_.top();
            queue_.pop();
            pending_tasks_.fetch_sub(1, std::memory_order_relaxed);
            running_tasks_.fetch_add(1, std::memory_order_relaxed);
        }

        // CRITICAL: NEVER hold queue_mutex_ or records_mutex_ while executing task callables (note: task.cancel() runs under queue_mutex_ during non-draining shutdown)
        execute_task(task);
    }
}

void TaskScheduler::execute_task(const Task& task) {
    auto record = task.record();
    if (!record) {
        return;
    }

    const auto start_sys = std::chrono::system_clock::now();
    const auto start_steady = std::chrono::steady_clock::now();

    // Fix 1: Write start_time and store TaskStatus::Running inside record->wait_mutex
    {
        std::lock_guard<std::mutex> lock(record->wait_mutex);
        record->start_time = start_sys;
        record->status.store(TaskStatus::Running, std::memory_order_release);
    }

    std::exception_ptr caught_exception = nullptr;
    try {
        task.execute();
    } catch (...) {
        caught_exception = std::current_exception();
    }

    const auto finish_sys = std::chrono::system_clock::now();
    const auto finish_steady = std::chrono::steady_clock::now();
    const auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        finish_steady - start_steady).count();

    {
        std::lock_guard<std::mutex> lock(record->wait_mutex);
        record->finish_time = finish_sys;
        if (caught_exception) {
            record->exception = caught_exception;
            record->status.store(TaskStatus::Failed, std::memory_order_release);
        } else {
            record->status.store(TaskStatus::Completed, std::memory_order_release);
        }
        record->wait_cv.notify_all();
    }

    // Atomically transition running -> completed/failed under queue_mutex_ to preserve coherent stats invariant
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (caught_exception) {
            failed_tasks_.fetch_add(1, std::memory_order_relaxed);
        } else {
            completed_tasks_.fetch_add(1, std::memory_order_relaxed);
        }
        total_execution_time_ns_.fetch_add(
            static_cast<uint64_t>(duration_ns), std::memory_order_relaxed);
        running_tasks_.fetch_sub(1, std::memory_order_relaxed);
        if (queue_.empty() && running_tasks_.load(std::memory_order_relaxed) == 0) {
            drain_cv_.notify_all();
        }
    }
}

std::optional<TaskStatus> TaskScheduler::get_task_status(TaskId id) const {
    auto record = find_record(id);
    if (!record) {
        return std::nullopt;
    }
    return record->status.load(std::memory_order_acquire);
}

bool TaskScheduler::wait_for_task(TaskId id, std::chrono::milliseconds timeout) const {
    auto record = find_record(id);
    if (!record) {
        throw std::invalid_argument("TaskScheduler::wait_for_task: Unknown task ID");
    }

    std::unique_lock<std::mutex> lock(record->wait_mutex);
    if (record->is_terminal()) {
        return true;
    }

    if (timeout == std::chrono::milliseconds::max()) {
        record->wait_cv.wait(lock, [&record]() { return record->is_terminal(); });
        return record->is_terminal();
    }

    return record->wait_cv.wait_for(lock, timeout, [&record]() { return record->is_terminal(); });
}

std::optional<TaskMetadata> TaskScheduler::get_task_metadata(TaskId id) const {
    auto record = find_record(id);
    if (!record) {
        return std::nullopt;
    }

    std::lock_guard<std::mutex> lock(record->wait_mutex);
    TaskMetadata meta;
    meta.id = record->id;
    meta.name = record->name;
    meta.priority = record->priority;
    meta.sequence_number = record->sequence_number;
    meta.submission_time = record->submission_time;
    meta.start_time = record->start_time;
    meta.finish_time = record->finish_time;
    meta.status = record->status.load(std::memory_order_acquire);
    meta.has_exception = (record->exception != nullptr);
    return meta;
}

std::exception_ptr TaskScheduler::get_task_exception(TaskId id) const {
    auto record = find_record(id);
    if (!record) {
        throw std::invalid_argument("TaskScheduler::get_task_exception: Unknown task ID");
    }
    if (!record->is_terminal()) {
        throw std::runtime_error("TaskScheduler::get_task_exception: Task is still pending or running");
    }

    std::lock_guard<std::mutex> lock(record->wait_mutex);
    return record->exception;
}

void TaskScheduler::rethrow_task_exception(TaskId id) const {
    auto exc = get_task_exception(id);
    if (exc) {
        std::rethrow_exception(exc);
    }
}

SchedulerStats TaskScheduler::get_stats() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    SchedulerStats stats;
    stats.total_accepted_tasks = total_accepted_tasks_.load(std::memory_order_relaxed);
    stats.pending_tasks = pending_tasks_.load(std::memory_order_relaxed);
    stats.running_tasks = running_tasks_.load(std::memory_order_relaxed);
    stats.completed_tasks = completed_tasks_.load(std::memory_order_relaxed);
    stats.failed_tasks = failed_tasks_.load(std::memory_order_relaxed);
    stats.cancelled_tasks = cancelled_tasks_.load(std::memory_order_relaxed);
    stats.rejected_submissions = rejected_submissions_.load(std::memory_order_relaxed);
    stats.total_execution_time_ns = total_execution_time_ns_.load(std::memory_order_relaxed);
    return stats;
}

bool TaskScheduler::is_idle() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return queue_.empty() && running_tasks_.load(std::memory_order_relaxed) == 0;
}

void TaskScheduler::wait_idle() {
    if (worker_pool_ && worker_pool_->is_current_thread_worker()) {
        throw std::logic_error(
            "TaskScheduler::wait_idle: Cannot be called from within a worker thread (deadlock prevention)");
    }
    std::unique_lock<std::mutex> lock(queue_mutex_);
    drain_cv_.wait(lock, [this]() {
        return queue_.empty() && running_tasks_.load(std::memory_order_relaxed) == 0;
    });
}

std::size_t TaskScheduler::queue_size() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return queue_.size();
}

void TaskScheduler::shutdown(bool drain) {
    if (worker_pool_ && worker_pool_->is_current_thread_worker()) {
        throw std::logic_error(
            "TaskScheduler::shutdown: Self-join deadlock detected. "
            "Graceful shutdown cannot be called from within a worker thread.");
    }

    std::unique_lock<std::mutex> shutdown_lock(shutdown_mutex_);
    if (is_shutdown_) {
        return;
    }

    accepting_tasks_.store(false, std::memory_order_release);
    drain_queued_.store(drain, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        stop_requested_.store(true, std::memory_order_release);

        if (!drain) {
            // Drain disabled: cancel all unstarted pending tasks immediately
            while (!queue_.empty()) {
                Task task = queue_.top();
                queue_.pop();
                pending_tasks_.fetch_sub(1, std::memory_order_relaxed);
                cancelled_tasks_.fetch_add(1, std::memory_order_relaxed);
                task.cancel();
            }
            // Fix 3: Wake up any threads waiting in wait_idle()
            drain_cv_.notify_all();
        }
    }

    queue_cv_.notify_all();

    if (worker_pool_) {
        worker_pool_->join_all();
    }

    is_shutdown_ = true;
}

std::size_t TaskScheduler::clear_history() {
    std::lock_guard<std::mutex> lock(records_mutex_);
    std::size_t cleared_count = 0;

    for (auto it = task_records_.begin(); it != task_records_.end(); ) {
        if (it->second->is_terminal()) {
            it = task_records_.erase(it);
            ++cleared_count;
        } else {
            ++it;
        }
    }
    return cleared_count;
}

std::size_t TaskScheduler::prune_history(std::size_t retain_count) {
    std::lock_guard<std::mutex> lock(records_mutex_);
    return prune_history_unlocked(retain_count);
}

std::size_t TaskScheduler::prune_history_unlocked(std::size_t retain_count) {
    std::vector<std::shared_ptr<TaskRecord>> terminal_records;
    terminal_records.reserve(task_records_.size());

    for (const auto& [id, record] : task_records_) {
        if (record->is_terminal()) {
            terminal_records.push_back(record);
        }
    }

    if (terminal_records.size() <= retain_count) {
        return 0;
    }

    // Sort ascending by sequence number so oldest records are pruned first
    std::sort(terminal_records.begin(), terminal_records.end(),
        [](const auto& a, const auto& b) {
            return a->sequence_number < b->sequence_number;
        });

    const std::size_t to_remove = terminal_records.size() - retain_count;
    for (std::size_t i = 0; i < to_remove; ++i) {
        task_records_.erase(terminal_records[i]->id);
    }

    return to_remove;
}

} // namespace threadforge
