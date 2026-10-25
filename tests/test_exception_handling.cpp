#include <threadforge/task_scheduler.hpp>

#include <gtest/gtest.h>
#include <stdexcept>
#include <string>

TEST(ExceptionHandlingTest, StandardExceptionCapturing) {
    threadforge::TaskScheduler scheduler(2);

    auto fut = scheduler.submit([]() -> int {
        throw std::invalid_argument("Invalid computational parameter");
    });

    EXPECT_THROW({
        fut.get();
    }, std::invalid_argument);

    EXPECT_EQ(fut.status(), threadforge::TaskStatus::Failed);

    auto status = scheduler.get_task_status(fut.id());
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, threadforge::TaskStatus::Failed);

    auto exc_ptr = scheduler.get_task_exception(fut.id());
    ASSERT_NE(exc_ptr, nullptr);

    try {
        std::rethrow_exception(exc_ptr);
    } catch (const std::invalid_argument& e) {
        EXPECT_STREQ(e.what(), "Invalid computational parameter");
    }
}

TEST(ExceptionHandlingTest, NonStandardExceptionSafety) {
    threadforge::TaskScheduler scheduler(2);

    // Throw a primitive integer as non-standard exception
    auto fut = scheduler.submit([]() {
        throw 404;
    });

    EXPECT_THROW({
        fut.get();
    }, int);

    EXPECT_EQ(fut.status(), threadforge::TaskStatus::Failed);

    auto exc_ptr = scheduler.get_task_exception(fut.id());
    ASSERT_NE(exc_ptr, nullptr);

    try {
        std::rethrow_exception(exc_ptr);
    } catch (int code) {
        EXPECT_EQ(code, 404);
    }
}

TEST(ExceptionHandlingTest, SubsequentTasksContinueAfterFailure) {
    threadforge::TaskScheduler scheduler(1); // Single worker proves same thread survives

    auto fut1 = scheduler.submit([]() {
        throw std::runtime_error("Fatal task explosion");
    });

    auto fut2 = scheduler.submit([]() {
        return 42;
    });

    EXPECT_THROW(fut1.get(), std::runtime_error);
    EXPECT_EQ(fut2.get(), 42);

    EXPECT_EQ(fut1.status(), threadforge::TaskStatus::Failed);
    EXPECT_EQ(fut2.status(), threadforge::TaskStatus::Completed);

    const auto stats = scheduler.get_stats();
    EXPECT_EQ(stats.total_accepted_tasks, 2u);
    EXPECT_EQ(stats.completed_tasks, 1u);
    EXPECT_EQ(stats.failed_tasks, 1u);
    EXPECT_TRUE(stats.invariant_holds());
}

TEST(ExceptionHandlingTest, QueryExceptionOnSuccessfulOrPendingTask) {
    threadforge::TaskScheduler scheduler(2);

    auto fut = scheduler.submit([]() {
        return "Success";
    });

    fut.get();

    // Querying exception for successful task must return nullptr
    auto exc_ptr = scheduler.get_task_exception(fut.id());
    EXPECT_EQ(exc_ptr, nullptr);
}
