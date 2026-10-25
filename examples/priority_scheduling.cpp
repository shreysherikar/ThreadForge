#include <threadforge/task_scheduler.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

int main() {
    std::cout << "=== ThreadForge Example 2: Priority Scheduling ===\n";

    // Single worker to observe deterministic queue dispatch order
    threadforge::TaskScheduler scheduler(1);

    std::mutex output_mutex;
    std::vector<std::string> execution_log;

    // Use a synchronization latch to hold the worker while we queue tasks
    std::atomic<bool> hold_worker{true};

    auto blocker = scheduler.submit([&hold_worker]() {
        while (hold_worker.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    // Submit tasks with various priorities
    scheduler.submit_named("Task_Low_1", threadforge::TaskPriority::Low, [&]() {
        std::lock_guard<std::mutex> lock(output_mutex);
        execution_log.push_back("Task_Low_1 (Priority: Low)");
    });

    scheduler.submit_named("Task_High_1", threadforge::TaskPriority::High, [&]() {
        std::lock_guard<std::mutex> lock(output_mutex);
        execution_log.push_back("Task_High_1 (Priority: High)");
    });

    scheduler.submit_named("Task_Critical_1", threadforge::TaskPriority::Critical, [&]() {
        std::lock_guard<std::mutex> lock(output_mutex);
        execution_log.push_back("Task_Critical_1 (Priority: Critical)");
    });

    scheduler.submit_named("Task_Normal_1", threadforge::TaskPriority::Normal, [&]() {
        std::lock_guard<std::mutex> lock(output_mutex);
        execution_log.push_back("Task_Normal_1 (Priority: Normal)");
    });

    scheduler.submit_named("Task_High_2", threadforge::TaskPriority::High, [&]() {
        std::lock_guard<std::mutex> lock(output_mutex);
        execution_log.push_back("Task_High_2 (Priority: High)");
    });

    std::cout << "All tasks queued. Releasing blocker task...\n";
    hold_worker.store(false, std::memory_order_release);

    scheduler.wait_idle();

    std::cout << "\nExecution Order (Observed):\n";
    for (std::size_t i = 0; i < execution_log.size(); ++i) {
        std::cout << "  [" << (i + 1) << "] " << execution_log[i] << "\n";
    }

    scheduler.shutdown(true);
    return 0;
}
