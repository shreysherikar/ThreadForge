#include <threadforge/task_scheduler.hpp>

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

TEST(TaskSchedulerBasicTest, RejectZeroWorkers) {
    EXPECT_THROW({
        threadforge::TaskScheduler scheduler(0);
    }, std::invalid_argument);
}

TEST(TaskSchedulerBasicTest, ConstructWithValidWorkers) {
    EXPECT_NO_THROW({
        threadforge::TaskScheduler scheduler(2);
        EXPECT_EQ(scheduler.worker_count(), 2u);
    });
}

TEST(TaskSchedulerBasicTest, SubmitAndExecuteSingleTask) {
    threadforge::TaskScheduler scheduler(2);

    auto future = scheduler.submit([]() {
        return 12345;
    });

    EXPECT_NE(future.id(), threadforge::kInvalidTaskId);
    EXPECT_TRUE(future.valid());

    int result = future.get();
    EXPECT_EQ(result, 12345);
    EXPECT_EQ(future.status(), threadforge::TaskStatus::Completed);
}

TEST(TaskSchedulerBasicTest, SubmitVoidTask) {
    threadforge::TaskScheduler scheduler(2);
    bool executed = false;

    auto future = scheduler.submit([&executed]() {
        executed = true;
    });

    future.get();
    EXPECT_TRUE(executed);
    EXPECT_EQ(future.status(), threadforge::TaskStatus::Completed);
}

TEST(TaskSchedulerBasicTest, TaskIdUniqueness) {
    threadforge::TaskScheduler scheduler(2);
    constexpr std::size_t kCount = 100;
    std::vector<threadforge::TaskId> ids;
    ids.reserve(kCount);

    for (std::size_t i = 0; i < kCount; ++i) {
        auto fut = scheduler.submit([]() { return 1; });
        ids.push_back(fut.id());
    }

    scheduler.wait_idle();

    // Verify all IDs are strictly unique and increasing
    for (std::size_t i = 1; i < ids.size(); ++i) {
        EXPECT_GT(ids[i], ids[i - 1]);
    }
}

TEST(TaskSchedulerBasicTest, WaitForTaskAndMetadata) {
    threadforge::TaskScheduler scheduler(2);

    auto fut = scheduler.submit_named(
        "TimedTask",
        threadforge::TaskPriority::Normal,
        []() {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return 999;
        });

    const threadforge::TaskId id = fut.id();
    bool finished = scheduler.wait_for_task(id, std::chrono::milliseconds(2000));
    EXPECT_TRUE(finished);

    auto meta = scheduler.get_task_metadata(id);
    ASSERT_TRUE(meta.has_value());
    EXPECT_EQ(meta->id, id);
    EXPECT_EQ(meta->name, "TimedTask");
    EXPECT_EQ(meta->priority, threadforge::TaskPriority::Normal);
    EXPECT_EQ(meta->status, threadforge::TaskStatus::Completed);
    EXPECT_FALSE(meta->has_exception);
    EXPECT_LE(meta->submission_time, meta->start_time);
    EXPECT_LE(meta->start_time, meta->finish_time);
}

TEST(TaskSchedulerBasicTest, UnknownTaskIdHandling) {
    threadforge::TaskScheduler scheduler(2);
    const threadforge::TaskId unknown_id = 9999999;

    EXPECT_FALSE(scheduler.get_task_status(unknown_id).has_value());
    EXPECT_FALSE(scheduler.get_task_metadata(unknown_id).has_value());
    EXPECT_THROW(scheduler.wait_for_task(unknown_id), std::invalid_argument);
    EXPECT_THROW((void)scheduler.get_task_exception(unknown_id), std::invalid_argument);
}

TEST(TaskSchedulerBasicTest, StatisticsAndInvariant) {
    threadforge::TaskScheduler scheduler(2);

    for (int i = 0; i < 20; ++i) {
        scheduler.submit([i]() {
            if (i % 5 == 0) {
                throw std::runtime_error("Simulated failure");
            }
        });
    }

    scheduler.wait_idle();

    const auto stats = scheduler.get_stats();
    EXPECT_EQ(stats.total_accepted_tasks, 20u);
    EXPECT_EQ(stats.pending_tasks, 0u);
    EXPECT_EQ(stats.running_tasks, 0u);
    EXPECT_EQ(stats.completed_tasks, 16u);
    EXPECT_EQ(stats.failed_tasks, 4u);
    EXPECT_EQ(stats.cancelled_tasks, 0u);
    EXPECT_EQ(stats.rejected_submissions, 0u);
    EXPECT_TRUE(stats.invariant_holds());
    EXPECT_GT(stats.total_execution_time_ns, 0u);
}

// Regression Test 1: After submit() returns, get_task_status(id) always has a value
TEST(TaskSchedulerRegressionTest, SubmitReturnsThenGetTaskStatusAlwaysHasValue) {
    threadforge::TaskScheduler scheduler(4);
    constexpr std::size_t kTaskCount = 500;
    std::vector<threadforge::TaskId> published_ids;
    published_ids.reserve(kTaskCount);
    std::mutex ids_mutex;
    std::atomic<bool> producer_done{false};

    // Concurrent reader thread continuously checks statuses of published tasks
    std::thread reader([&]() {
        std::size_t checked_index = 0;
        while (!producer_done.load(std::memory_order_acquire) || checked_index < kTaskCount) {
            threadforge::TaskId id_to_check = threadforge::kInvalidTaskId;
            {
                std::lock_guard<std::mutex> lock(ids_mutex);
                if (checked_index < published_ids.size()) {
                    id_to_check = published_ids[checked_index++];
                }
            }
            if (id_to_check != threadforge::kInvalidTaskId) {
                auto status = scheduler.get_task_status(id_to_check);
                // Invariant: The record MUST exist in task_records_ immediately upon submission return
                EXPECT_TRUE(status.has_value());
            } else {
                std::this_thread::yield();
            }
        }
    });

    // Producer submits tasks and immediately records their IDs
    for (std::size_t i = 0; i < kTaskCount; ++i) {
        auto fut = scheduler.submit([i]() { return i * 2; });
        {
            std::lock_guard<std::mutex> lock(ids_mutex);
            published_ids.push_back(fut.id());
        }
    }
    producer_done.store(true, std::memory_order_release);

    reader.join();
    scheduler.wait_idle();
}

// Regression Test 2: Concurrent get_task_metadata() while task starts has no data race
TEST(TaskSchedulerRegressionTest, ConcurrentGetTaskMetadataWhileTaskStarts) {
    threadforge::TaskScheduler scheduler(1);
    auto start_gate_promise = std::make_shared<std::promise<void>>();
    auto start_gate_future = start_gate_promise->get_future().share();
    auto task_running_promise = std::make_shared<std::promise<void>>();
    auto task_running_future = task_running_promise->get_future().share();

    auto fut = scheduler.submit([start_gate_future, task_running_promise]() mutable {
        task_running_promise->set_value();
        start_gate_future.wait();
        return 42;
    });

    const auto task_id = fut.id();
    std::atomic<bool> stop_reader{false};

    // Reader thread rapidly queries metadata while task is transitioning Pending -> Running
    std::thread metadata_reader([&]() {
        while (!stop_reader.load(std::memory_order_relaxed)) {
            auto meta = scheduler.get_task_metadata(task_id);
            if (meta.has_value()) {
                EXPECT_EQ(meta->id, task_id);
            }
        }
    });

    // Wait until task is executing
    task_running_future.wait();
    // Allow task to finish
    start_gate_promise->set_value();
    fut.wait();

    stop_reader.store(true, std::memory_order_relaxed);
    metadata_reader.join();
    scheduler.wait_idle();
}

// Regression Test 4: After future.get() returns, get_task_status(id) is terminal
TEST(TaskSchedulerRegressionTest, AfterFutureGetReturnsGetTaskStatusIsTerminal) {
    threadforge::TaskScheduler scheduler(2);

    for (int i = 0; i < 50; ++i) {
        if (i % 2 == 0) {
            auto fut = scheduler.submit([i]() { return i * 10; });
            int val = fut.get();
            EXPECT_EQ(val, i * 10);
            auto status = scheduler.get_task_status(fut.id());
            ASSERT_TRUE(status.has_value());
            EXPECT_TRUE(threadforge::is_terminal(*status));
            EXPECT_EQ(*status, threadforge::TaskStatus::Completed);
            EXPECT_EQ(fut.status(), threadforge::TaskStatus::Completed);
        } else {
            auto fut = scheduler.submit([]() {
                throw std::runtime_error("Simulated regression test failure");
            });
            EXPECT_THROW(fut.get(), std::runtime_error);
            auto status = scheduler.get_task_status(fut.id());
            ASSERT_TRUE(status.has_value());
            EXPECT_TRUE(threadforge::is_terminal(*status));
            EXPECT_EQ(*status, threadforge::TaskStatus::Failed);
            EXPECT_EQ(fut.status(), threadforge::TaskStatus::Failed);
        }
    }
    scheduler.wait_idle();
}
