#pragma once

#include <cstdint>
#include <chrono>
#include <ostream>

namespace threadforge {

/**
 * @brief Thread-safe aggregate statistics snapshot of the scheduler.
 *
 * Provides counters and gauges detailing task submissions, active workloads,
 * completions, failures, cancellations, and rejection metrics.
 *
 * Invariant:
 *   total_accepted_tasks == pending_tasks + running_tasks + completed_tasks + failed_tasks + cancelled_tasks
 */
struct SchedulerStats {
    uint64_t total_accepted_tasks{0};     ///< Cumulative tasks accepted into the scheduler.
    uint64_t pending_tasks{0};            ///< Current count of tasks waiting in queue (gauge).
    uint64_t running_tasks{0};            ///< Current count of tasks actively running (gauge).
    uint64_t completed_tasks{0};          ///< Cumulative tasks that completed successfully.
    uint64_t failed_tasks{0};             ///< Cumulative tasks that terminated with an exception.
    uint64_t cancelled_tasks{0};          ///< Cumulative tasks cancelled before execution started.
    uint64_t rejected_submissions{0};     ///< Submissions rejected (e.g. after shutdown).
    uint64_t total_execution_time_ns{0};  ///< Cumulative execution duration in nanoseconds.

    /**
     * @brief Verifies the fundamental scheduler invariant for accepted tasks.
     */
    [[nodiscard]] constexpr bool invariant_holds() const noexcept {
        return total_accepted_tasks == (pending_tasks + running_tasks + completed_tasks + failed_tasks + cancelled_tasks);
    }

    /**
     * @brief Computes average execution time per finished task in microseconds.
     */
    [[nodiscard]] double average_execution_time_us() const noexcept {
        const uint64_t finished = completed_tasks + failed_tasks;
        if (finished == 0) {
            return 0.0;
        }
        return static_cast<double>(total_execution_time_ns) / (static_cast<double>(finished) * 1000.0);
    }
};

/**
 * @brief Stream insertion operator for SchedulerStats.
 */
inline std::ostream& operator<<(std::ostream& os, const SchedulerStats& stats) {
    os << "SchedulerStats{"
       << "accepted=" << stats.total_accepted_tasks
       << ", pending=" << stats.pending_tasks
       << ", running=" << stats.running_tasks
       << ", completed=" << stats.completed_tasks
       << ", failed=" << stats.failed_tasks
       << ", cancelled=" << stats.cancelled_tasks
       << ", rejected=" << stats.rejected_submissions
       << ", total_exec_time_ns=" << stats.total_execution_time_ns
       << "}";
    return os;
}

} // namespace threadforge
