#include <threadforge/task_scheduler.hpp>

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

TEST(ShutdownTest, EmptySchedulerShutdown) {
    threadforge::TaskScheduler scheduler(3);
    EXPECT_NO_THROW({
        scheduler.shutdown(true);
    });
    EXPECT_FALSE(scheduler.is_accepting_tasks());
}

TEST(ShutdownTest, GracefulDrainExecutesAllQueuedTasks) {
    threadforge::TaskScheduler scheduler(2);
    constexpr int kTaskCount = 20;
    std::atomic<int> executed_count{0};

    for (int i = 0; i < kTaskCount; ++i) {
        scheduler.submit([&executed_count]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            executed_count.fetch_add(1, std::memory_order_relaxed);
        });
    }

    scheduler.shutdown(true);

    EXPECT_EQ(executed_count.load(), kTaskCount);
    const auto stats = scheduler.get_stats();
    EXPECT_EQ(stats.completed_tasks, static_cast<uint64_t>(kTaskCount));
    EXPECT_EQ(stats.pending_tasks, 0u);
    EXPECT_EQ(stats.running_tasks, 0u);
    EXPECT_EQ(stats.cancelled_tasks, 0u);
    EXPECT_TRUE(stats.invariant_holds());
}

TEST(ShutdownTest, NonDrainingShutdownCancelsQueuedWork) {
    threadforge::TaskScheduler scheduler(1); // Single worker

    std::atomic<bool> blocker_active{true};
    std::atomic<bool> blocker_running{false};

    // Hold the worker
    auto blocker = scheduler.submit([&blocker_active, &blocker_running]() {
        blocker_running.store(true, std::memory_order_release);
        while (blocker_active.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    // Ensure the blocker is actively running on the single worker
    while (!blocker_running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Queue 5 tasks that won't start yet
    std::atomic<int> unstarted_executions{0};
    std::vector<threadforge::TaskFuture<int>> unstarted_futures;
    unstarted_futures.reserve(5);

    for (int i = 0; i < 5; ++i) {
        unstarted_futures.push_back(scheduler.submit([&unstarted_executions, i]() {
            unstarted_executions.fetch_add(1, std::memory_order_relaxed);
            return i;
        }));
    }

    // Release blocker and shut down without draining
    blocker_active.store(false, std::memory_order_release);
    scheduler.shutdown(false);

    // Unstarted tasks were not executed
    EXPECT_EQ(unstarted_executions.load(), 0);

    const auto stats = scheduler.get_stats();
    EXPECT_EQ(stats.cancelled_tasks, 5u);
    EXPECT_EQ(stats.failed_tasks, 0u);
    EXPECT_EQ(stats.completed_tasks, 1u); // The blocker
    EXPECT_TRUE(stats.invariant_holds());

    // Verify all cancelled futures throw TaskCancelledException and report Cancelled status
    for (auto& fut : unstarted_futures) {
        EXPECT_EQ(fut.status(), threadforge::TaskStatus::Cancelled);
        EXPECT_THROW(fut.get(), threadforge::TaskCancelledException);
        auto status = scheduler.get_task_status(fut.id());
        ASSERT_TRUE(status.has_value());
        EXPECT_EQ(*status, threadforge::TaskStatus::Cancelled);
    }
}

TEST(ShutdownTest, RepeatedShutdownIsIdempotent) {
    threadforge::TaskScheduler scheduler(2);
    EXPECT_NO_THROW({
        scheduler.shutdown(true);
        scheduler.shutdown(true);
        scheduler.shutdown(false);
    });
}

TEST(ShutdownTest, ConcurrentShutdownCalls) {
    threadforge::TaskScheduler scheduler(4);
    constexpr std::size_t kThreads = 6;
    std::vector<std::thread> shutdown_callers;
    shutdown_callers.reserve(kThreads);

    for (std::size_t i = 0; i < kThreads; ++i) {
        shutdown_callers.emplace_back([&scheduler]() {
            scheduler.shutdown(true);
        });
    }

    for (auto& t : shutdown_callers) {
        t.join();
    }

    EXPECT_FALSE(scheduler.is_accepting_tasks());
}

TEST(ShutdownTest, SubmissionsAfterShutdownRejected) {
    threadforge::TaskScheduler scheduler(2);
    scheduler.shutdown(true);

    EXPECT_THROW({
        scheduler.submit([]() { return 1; });
    }, threadforge::SchedulerShutdownException);

    const auto stats = scheduler.get_stats();
    EXPECT_EQ(stats.rejected_submissions, 1u);
}

TEST(ShutdownTest, PreventWorkerSelfJoinDeadlock) {
    threadforge::TaskScheduler scheduler(2);

    std::atomic<bool> caught_expected_exception{false};

    auto fut = scheduler.submit([&scheduler, &caught_expected_exception]() {
        try {
            // Attempting to invoke shutdown from within a worker thread
            scheduler.shutdown(true);
        } catch (const std::logic_error& e) {
            caught_expected_exception.store(true, std::memory_order_release);
        }
    });

    fut.get();
    EXPECT_TRUE(caught_expected_exception.load(std::memory_order_acquire));

    scheduler.shutdown(true);
}

TEST(ShutdownTest, PreventWorkerWaitIdleSelfDeadlock) {
    threadforge::TaskScheduler scheduler(2);

    std::atomic<bool> caught_expected_exception{false};

    auto fut = scheduler.submit([&scheduler, &caught_expected_exception]() {
        try {
            // Attempting to invoke wait_idle from within a worker thread
            scheduler.wait_idle();
        } catch (const std::logic_error& e) {
            caught_expected_exception.store(true, std::memory_order_release);
        }
    });

    fut.get();
    EXPECT_TRUE(caught_expected_exception.load(std::memory_order_acquire));

    scheduler.shutdown(true);
}

// Regression Test 3: wait_idle() called from another thread returns after shutdown(false) cancels queued tasks
TEST(ShutdownTest, WaitIdleWakesUpOnNonDrainingShutdown) {
    threadforge::TaskScheduler scheduler(1);
    auto blocker_started_promise = std::make_shared<std::promise<void>>();
    auto blocker_started_future = blocker_started_promise->get_future().share();
    std::atomic<bool> blocker_can_exit{false};

    // 1. Blocker task holds the single worker
    auto blocker = scheduler.submit([blocker_started_promise, &blocker_can_exit]() mutable {
        blocker_started_promise->set_value();
        while (!blocker_can_exit.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    // Wait until blocker is actively running
    blocker_started_future.wait();

    // 2. Queue 5 tasks that will remain pending
    for (int i = 0; i < 5; ++i) {
        scheduler.submit([]() { return 100; });
    }

    // 3. Thread B calls wait_idle()
    std::atomic<bool> wait_idle_completed{false};
    std::thread wait_idle_thread([&]() {
        scheduler.wait_idle();
        wait_idle_completed.store(true, std::memory_order_release);
    });

    // 4. Thread C releases the blocker only after shutdown(false) cancels the queued tasks
    std::thread releaser([&]() {
        while (scheduler.get_stats().cancelled_tasks < 5) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        blocker_can_exit.store(true, std::memory_order_release);
    });

    // 5. Main thread triggers immediate shutdown(false), cancelling queued tasks
    scheduler.shutdown(false);

    releaser.join();
    wait_idle_thread.join();
    EXPECT_TRUE(wait_idle_completed.load(std::memory_order_acquire));

    const auto stats = scheduler.get_stats();
    EXPECT_EQ(stats.cancelled_tasks, 5u);
    EXPECT_EQ(stats.completed_tasks, 1u);
    EXPECT_TRUE(stats.invariant_holds());
}
