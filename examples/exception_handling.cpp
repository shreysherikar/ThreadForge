#include <threadforge/task_scheduler.hpp>

#include <iostream>
#include <stdexcept>
#include <string>

int main() {
    std::cout << "=== ThreadForge Example 4: Exception Handling and Resilience ===\n";

    threadforge::TaskScheduler scheduler(2);

    // 1. Submit a task throwing std::runtime_error
    auto future_failing = scheduler.submit([]() -> int {
        std::cout << "[Worker] Executing task destined to fail...\n";
        throw std::runtime_error("Simulated computational failure: divide by zero");
    });

    // 2. Submit a subsequent healthy task to prove workers remain alive and functional
    auto future_healthy = scheduler.submit([]() -> std::string {
        return "Worker is healthy and processing following tasks normally!";
    });

    // Attempt to retrieve result from failing task
    try {
        std::cout << "Awaiting failing task result...\n";
        future_failing.get();
    } catch (const std::exception& e) {
        std::cout << "Successfully caught task exception from future.get():\n  "
                  << e.what() << "\n";
    }

    // Inspect task status from scheduler
    auto status = scheduler.get_task_status(future_failing.id());
    if (status) {
        std::cout << "Task status reported by scheduler: "
                  << threadforge::to_string(*status) << "\n";
    }

    // Retrieve exception directly from scheduler
    auto exc_ptr = scheduler.get_task_exception(future_failing.id());
    if (exc_ptr) {
        try {
            std::rethrow_exception(exc_ptr);
        } catch (const std::exception& e) {
            std::cout << "Re-thrown from scheduler exception registry: "
                      << e.what() << "\n";
        }
    }

    // Retrieve result from healthy task
    std::cout << "Healthy task result: " << future_healthy.get() << "\n";

    const auto stats = scheduler.get_stats();
    std::cout << "Stats verification:\n"
              << "  Accepted:  " << stats.total_accepted_tasks << "\n"
              << "  Completed: " << stats.completed_tasks << "\n"
              << "  Failed:    " << stats.failed_tasks << "\n";

    scheduler.shutdown(true);
    return 0;
}
