#include <threadforge/task_scheduler.hpp>

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

TEST(PriorityQueueTest, HigherPriorityExecutesBeforeLowerPriority) {
    // Single worker enables deterministic queue dispatch observation
    threadforge::TaskScheduler scheduler(1);

    std::atomic<bool> blocker_gate{true};
    std::mutex order_mutex;
    std::vector<int> execution_order;

    // Blocker task keeps the single worker busy while tasks are queued
    auto blocker = scheduler.submit([&blocker_gate]() {
        while (blocker_gate.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    // Submit Low priority first, then High priority second
    scheduler.submit_with_priority(threadforge::TaskPriority::Low, [&]() {
        std::lock_guard<std::mutex> lock(order_mutex);
        execution_order.push_back(1); // Low
    });

    scheduler.submit_with_priority(threadforge::TaskPriority::High, [&]() {
        std::lock_guard<std::mutex> lock(order_mutex);
        execution_order.push_back(2); // High
    });

    // Release blocker
    blocker_gate.store(false, std::memory_order_release);
    scheduler.wait_idle();

    ASSERT_EQ(execution_order.size(), 2u);
    // High priority (2) must execute before Low priority (1)
    EXPECT_EQ(execution_order[0], 2);
    EXPECT_EQ(execution_order[1], 1);
}

TEST(PriorityQueueTest, EqualPriorityPreservesFIFOOrder) {
    threadforge::TaskScheduler scheduler(1);

    std::atomic<bool> blocker_gate{true};
    std::mutex order_mutex;
    std::vector<int> execution_order;

    auto blocker = scheduler.submit([&blocker_gate]() {
        while (blocker_gate.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    // Submit 5 tasks with identical Normal priority
    for (int i = 0; i < 5; ++i) {
        scheduler.submit_with_priority(threadforge::TaskPriority::Normal, [&, i]() {
            std::lock_guard<std::mutex> lock(order_mutex);
            execution_order.push_back(i);
        });
    }

    blocker_gate.store(false, std::memory_order_release);
    scheduler.wait_idle();

    ASSERT_EQ(execution_order.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(execution_order[i], i);
    }
}

TEST(PriorityQueueTest, MultiLevelPriorityOrdering) {
    threadforge::TaskScheduler scheduler(1);

    std::atomic<bool> blocker_gate{true};
    std::mutex order_mutex;
    std::vector<std::string> order;

    auto blocker = scheduler.submit([&blocker_gate]() {
        while (blocker_gate.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    // Submit in reverse priority order: Low, Normal, High, Critical
    scheduler.submit_named("Low", threadforge::TaskPriority::Low, [&]() {
        std::lock_guard<std::mutex> lock(order_mutex);
        order.push_back("Low");
    });

    scheduler.submit_named("Normal", threadforge::TaskPriority::Normal, [&]() {
        std::lock_guard<std::mutex> lock(order_mutex);
        order.push_back("Normal");
    });

    scheduler.submit_named("High", threadforge::TaskPriority::High, [&]() {
        std::lock_guard<std::mutex> lock(order_mutex);
        order.push_back("High");
    });

    scheduler.submit_named("Critical", threadforge::TaskPriority::Critical, [&]() {
        std::lock_guard<std::mutex> lock(order_mutex);
        order.push_back("Critical");
    });

    blocker_gate.store(false, std::memory_order_release);
    scheduler.wait_idle();

    ASSERT_EQ(order.size(), 4u);
    EXPECT_EQ(order[0], "Critical");
    EXPECT_EQ(order[1], "High");
    EXPECT_EQ(order[2], "Normal");
    EXPECT_EQ(order[3], "Low");
}

TEST(PriorityQueueTest, NonPreemptiveExecutionBehavior) {
    // Proves that when a task is Running, submitting a Critical task does NOT
    // preempt or abort the running task.
    threadforge::TaskScheduler scheduler(1);

    std::atomic<bool> running_task_started{false};
    std::atomic<bool> running_task_can_finish{false};
    std::vector<std::string> sequence;
    std::mutex seq_mutex;

    // Start a low-priority task
    scheduler.submit_named("LongLow", threadforge::TaskPriority::Low, [&]() {
        running_task_started.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(seq_mutex);
            sequence.push_back("LongLow Started");
        }
        while (!running_task_can_finish.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        {
            std::lock_guard<std::mutex> lock(seq_mutex);
            sequence.push_back("LongLow Finished");
        }
    });

    // Wait until LongLow is actively running on the worker
    while (!running_task_started.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Now submit a Critical task
    scheduler.submit_named("UrgentCritical", threadforge::TaskPriority::Critical, [&]() {
        std::lock_guard<std::mutex> lock(seq_mutex);
        sequence.push_back("UrgentCritical Executed");
    });

    // Allow LongLow to conclude
    running_task_can_finish.store(true, std::memory_order_release);
    scheduler.wait_idle();

    ASSERT_EQ(sequence.size(), 3u);
    // LongLow was NOT preempted: it completed before UrgentCritical started
    EXPECT_EQ(sequence[0], "LongLow Started");
    EXPECT_EQ(sequence[1], "LongLow Finished");
    EXPECT_EQ(sequence[2], "UrgentCritical Executed");
}
