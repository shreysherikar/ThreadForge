#include <threadforge/task_scheduler.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

int main() {
    std::cout << "=== ThreadForge Example 3: Concurrent Producer Submissions ===\n";

    constexpr std::size_t kNumProducers = 8;
    constexpr std::size_t kTasksPerProducer = 250;
    constexpr std::size_t kExpectedTotalTasks = kNumProducers * kTasksPerProducer;

    threadforge::TaskScheduler scheduler(4);
    std::atomic<uint64_t> completed_counter{0};

    std::vector<std::thread> producers;
    producers.reserve(kNumProducers);

    const auto start_time = std::chrono::steady_clock::now();

    for (std::size_t p = 0; p < kNumProducers; ++p) {
        producers.emplace_back([&scheduler, &completed_counter, p]() {
            for (std::size_t i = 0; i < kTasksPerProducer; ++i) {
                const auto priority = (i % 3 == 0) ? threadforge::TaskPriority::High
                                    : (i % 2 == 0) ? threadforge::TaskPriority::Normal
                                                   : threadforge::TaskPriority::Low;
                scheduler.submit_with_priority(priority, [&completed_counter]() {
                    completed_counter.fetch_add(1, std::memory_order_relaxed);
                });
            }
        });
    }

    for (auto& producer : producers) {
        producer.join();
    }

    std::cout << "All " << kNumProducers << " producer threads finished submission.\n";
    std::cout << "Waiting for scheduler to drain and become idle...\n";

    scheduler.wait_idle();

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time).count();

    const auto stats = scheduler.get_stats();
    std::cout << "Verification Results:\n"
              << "  Expected Executions: " << kExpectedTotalTasks << "\n"
              << "  Completed Counter:   " << completed_counter.load() << "\n"
              << "  Stats Accepted:      " << stats.total_accepted_tasks << "\n"
              << "  Stats Completed:     " << stats.completed_tasks << "\n"
              << "  Total Elapsed Time:  " << elapsed << " ms\n"
              << "  Status Invariant:    " << (stats.invariant_holds() ? "VERIFIED" : "FAILED") << "\n";

    scheduler.shutdown(true);
    return 0;
}
