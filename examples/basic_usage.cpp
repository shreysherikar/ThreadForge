#include <threadforge/task_scheduler.hpp>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

int main() {
    std::cout << "=== ThreadForge Example 1: Basic Usage ===\n";

    // Instantiate scheduler with 4 worker threads
    threadforge::TaskScheduler scheduler(4);

    // 1. Submit a task returning a typed result (int)
    auto future_int = scheduler.submit([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return 42 * 2;
    });

    // 2. Submit a named task returning a string
    auto future_str = scheduler.submit_named(
        "ComputeGreeting",
        threadforge::TaskPriority::Normal,
        [](const std::string& name) {
            return "Hello, " + name + " from ThreadForge!";
        },
        "Systems Engineer");

    // 3. Submit a void-returning task
    auto future_void = scheduler.submit([]() {
        std::cout << "[Worker] Void task executing successfully.\n";
    });

    // Wait and retrieve results
    std::cout << "Computed integer result: " << future_int.get() << "\n";
    std::cout << "Greeting string result: " << future_str.get() << "\n";
    future_void.get(); // Validates completion

    // Query task status
    auto status = scheduler.get_task_status(future_int.id());
    if (status) {
        std::cout << "Task " << future_int.id() << " terminal status: "
                  << threadforge::to_string(*status) << "\n";
    }

    // Inspect scheduler statistics
    const auto stats = scheduler.get_stats();
    std::cout << "Scheduler Statistics:\n"
              << "  Total Accepted: " << stats.total_accepted_tasks << "\n"
              << "  Completed:      " << stats.completed_tasks << "\n"
              << "  Failed:         " << stats.failed_tasks << "\n"
              << "  Pending:        " << stats.pending_tasks << "\n"
              << "  Running:        " << stats.running_tasks << "\n"
              << "  Invariant holds: " << (stats.invariant_holds() ? "YES" : "NO") << "\n";

    scheduler.shutdown(true);
    std::cout << "Scheduler shut down cleanly.\n";
    return 0;
}
