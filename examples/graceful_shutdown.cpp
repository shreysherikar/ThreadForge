#include <threadforge/task_scheduler.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

int main() {
    std::cout << "=== ThreadForge Example 5: Graceful Shutdown ===\n";

    threadforge::TaskScheduler scheduler(2);
    std::atomic<int> completed_count{0};

    constexpr int kQueuedTasks = 10;
    std::cout << "Submitting " << kQueuedTasks << " tasks...\n";

    for (int i = 0; i < kQueuedTasks; ++i) {
        scheduler.submit([&completed_count, i]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            completed_count.fetch_add(1, std::memory_order_relaxed);
        });
    }

    std::cout << "Initiating graceful shutdown (drain = true)...\n";
    // Shutdown drains all 10 tasks and joins workers before returning
    scheduler.shutdown(true);

    std::cout << "Shutdown completed.\n";
    std::cout << "Total tasks completed during drain: " << completed_count.load()
              << " / " << kQueuedTasks << "\n";

    // Attempting to submit after shutdown must throw SchedulerShutdownException
    try {
        std::cout << "Attempting submission post-shutdown...\n";
        scheduler.submit([]() { return 1; });
        std::cerr << "ERROR: Submission should have been rejected!\n";
    } catch (const threadforge::SchedulerShutdownException& e) {
        std::cout << "Successfully caught expected exception:\n  " << e.what() << "\n";
    }

    const auto stats = scheduler.get_stats();
    std::cout << "Final Stats:\n"
              << "  Accepted: " << stats.total_accepted_tasks << "\n"
              << "  Completed: " << stats.completed_tasks << "\n"
              << "  Rejected: " << stats.rejected_submissions << "\n";

    return 0;
}
