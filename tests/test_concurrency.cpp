#include <threadforge/task_scheduler.hpp>

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

namespace {

/**
 * @brief Test-only synchronization barrier implemented using C++17 primitives.
 */
class TestBarrier {
public:
    explicit TestBarrier(std::size_t count) : threshold_(count), count_(count), generation_(0) {}

    void arrive_and_wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto gen = generation_;
        if (--count_ == 0) {
            generation_++;
            count_ = threshold_;
            cv_.notify_all();
        } else {
            cv_.wait(lock, [this, gen]() { return gen != generation_; });
        }
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t threshold_;
    std::size_t count_;
    std::size_t generation_;
};

} // namespace

TEST(ConcurrencyTest, ExactlyOnceExecutionUnderMultiProducers) {
    constexpr std::size_t kProducers = 6;
    constexpr std::size_t kTasksPerProducer = 500;
    constexpr std::size_t kTotalTasks = kProducers * kTasksPerProducer;

    threadforge::TaskScheduler scheduler(4);
    std::atomic<uint64_t> execution_counter{0};

    std::vector<std::thread> producers;
    producers.reserve(kProducers);

    for (std::size_t p = 0; p < kProducers; ++p) {
        producers.emplace_back([&scheduler, &execution_counter]() {
            for (std::size_t i = 0; i < kTasksPerProducer; ++i) {
                scheduler.submit([&execution_counter]() {
                    execution_counter.fetch_add(1, std::memory_order_relaxed);
                });
            }
        });
    }

    for (auto& t : producers) {
        t.join();
    }

    scheduler.wait_idle();

    EXPECT_EQ(execution_counter.load(), kTotalTasks);
    const auto stats = scheduler.get_stats();
    EXPECT_EQ(stats.total_accepted_tasks, kTotalTasks);
    EXPECT_EQ(stats.completed_tasks, kTotalTasks);
    EXPECT_EQ(stats.failed_tasks, 0u);
    EXPECT_TRUE(stats.invariant_holds());
}

TEST(ConcurrencyTest, OverlappingWorkerExecutionVerifiedViaBarrier) {
    constexpr std::size_t kWorkers = 4;
    threadforge::TaskScheduler scheduler(kWorkers);

    TestBarrier barrier(kWorkers);
    std::atomic<std::size_t> concurrent_threads{0};
    std::atomic<std::size_t> max_concurrent_observed{0};

    for (std::size_t i = 0; i < kWorkers; ++i) {
        scheduler.submit([&]() {
            const auto current = concurrent_threads.fetch_add(1, std::memory_order_acq_rel) + 1;
            // Update maximum concurrent threads observed
            auto prev_max = max_concurrent_observed.load(std::memory_order_relaxed);
            while (current > prev_max &&
                   !max_concurrent_observed.compare_exchange_weak(prev_max, current)) {}

            // Synchronize all kWorkers simultaneously inside their executing tasks
            barrier.arrive_and_wait();

            concurrent_threads.fetch_sub(1, std::memory_order_acq_rel);
        });
    }

    scheduler.wait_idle();
    EXPECT_EQ(max_concurrent_observed.load(), kWorkers);
}

TEST(ConcurrencyTest, ConcurrentStatusAndStatsQueries) {
    threadforge::TaskScheduler scheduler(4);
    std::atomic<bool> stop_flag{false};
    constexpr std::size_t kTasks = 300;

    std::vector<threadforge::TaskId> task_ids;
    task_ids.reserve(kTasks);

    for (std::size_t i = 0; i < kTasks; ++i) {
        auto fut = scheduler.submit([]() {
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        });
        task_ids.push_back(fut.id());
    }

    // Thread querying statuses concurrently
    std::thread status_reader([&]() {
        while (!stop_flag.load(std::memory_order_relaxed)) {
            for (auto id : task_ids) {
                auto st = scheduler.get_task_status(id);
                // Status should be valid or present
                if (st.has_value()) {
                    EXPECT_LE(static_cast<int>(*st), static_cast<int>(threadforge::TaskStatus::Cancelled));
                }
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    // Thread querying stats snapshots concurrently
    std::thread stats_reader([&]() {
        while (!stop_flag.load(std::memory_order_relaxed)) {
            auto stats = scheduler.get_stats();
            EXPECT_TRUE(stats.invariant_holds());
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    scheduler.wait_idle();
    stop_flag.store(true, std::memory_order_relaxed);
    status_reader.join();
    stats_reader.join();

    const auto final_stats = scheduler.get_stats();
    EXPECT_EQ(final_stats.completed_tasks, kTasks);
    EXPECT_TRUE(final_stats.invariant_holds());
}
