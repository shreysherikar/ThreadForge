#pragma once

#include <cstdint>
#include <string_view>
#include <ostream>

namespace threadforge {

/**
 * @brief Represents the lifecycle states of a task within ThreadForge.
 *
 * State transitions follow a deterministic DAG (Directed Acyclic Graph):
 *   Pending -> Running -> Completed
 *                       \-> Failed
 *           \-> Cancelled
 *
 * All state transitions are thread-safe and atomic.
 */
enum class TaskStatus : uint8_t {
    Pending = 0,   ///< Task is queued and awaiting an available worker.
    Running = 1,   ///< Task has been dequeued and is executing on a worker thread.
    Completed = 2, ///< Task completed execution successfully without unhandled exceptions.
    Failed = 3,    ///< Task terminated due to an uncaught exception.
    Cancelled = 4  ///< Task was cancelled before execution started (e.g. non-draining shutdown).
};

/**
 * @brief Checks if a given status represents a terminal state.
 *
 * Terminal states (Completed, Failed, Cancelled) indicate that task lifecycle
 * has concluded and no further state transitions will occur.
 */
constexpr bool is_terminal(TaskStatus status) noexcept {
    return status == TaskStatus::Completed ||
           status == TaskStatus::Failed ||
           status == TaskStatus::Cancelled;
}

/**
 * @brief Converts a TaskStatus enum value to a human-readable string view.
 */
constexpr std::string_view to_string(TaskStatus status) noexcept {
    switch (status) {
        case TaskStatus::Pending:
            return "Pending";
        case TaskStatus::Running:
            return "Running";
        case TaskStatus::Completed:
            return "Completed";
        case TaskStatus::Failed:
            return "Failed";
        case TaskStatus::Cancelled:
            return "Cancelled";
        default:
            return "Unknown";
    }
}

/**
 * @brief Stream insertion operator for TaskStatus.
 */
inline std::ostream& operator<<(std::ostream& os, TaskStatus status) {
    return os << to_string(status);
}

} // namespace threadforge
