#pragma once

#include <cstdint>
#include <string_view>
#include <ostream>

namespace threadforge {

/**
 * @brief Priority levels for scheduled tasks.
 *
 * Higher numeric values correspond to higher scheduling priority.
 * When workers select tasks from the internal priority queue, tasks with
 * higher priority are dispatched before tasks with lower priority.
 * For tasks with identical priority, strict FIFO (First-In-First-Out)
 * ordering is maintained via sequence numbers.
 */
enum class TaskPriority : int32_t {
    Low = 0,
    Normal = 1,
    High = 2,
    Critical = 3
};

/**
 * @brief Converts a TaskPriority enum value to a human-readable string view.
 */
constexpr std::string_view to_string(TaskPriority priority) noexcept {
    switch (priority) {
        case TaskPriority::Low:
            return "Low";
        case TaskPriority::Normal:
            return "Normal";
        case TaskPriority::High:
            return "High";
        case TaskPriority::Critical:
            return "Critical";
        default:
            return "Unknown";
    }
}

/**
 * @brief Stream insertion operator for TaskPriority.
 */
inline std::ostream& operator<<(std::ostream& os, TaskPriority priority) {
    return os << to_string(priority);
}

} // namespace threadforge
