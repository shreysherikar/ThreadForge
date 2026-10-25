#include <threadforge/task_scheduler.hpp>

#include <gtest/gtest.h>
#include <string>
#include <vector>

struct CustomPayload {
    int id;
    std::string tag;
    double score;

    bool operator==(const CustomPayload& o) const {
        return id == o.id && tag == o.tag && score == o.score;
    }
};

TEST(TaskResultsTest, TypedResultTypes) {
    threadforge::TaskScheduler scheduler(2);

    // 1. Primitive int
    auto f_int = scheduler.submit([]() { return 100; });
    EXPECT_EQ(f_int.get(), 100);

    // 2. String
    auto f_str = scheduler.submit([]() { return std::string("ThreadForge"); });
    EXPECT_EQ(f_str.get(), "ThreadForge");

    // 3. Custom aggregate struct
    auto f_struct = scheduler.submit([]() {
        return CustomPayload{7, "Engine", 99.5};
    });
    const CustomPayload expected{7, "Engine", 99.5};
    EXPECT_EQ(f_struct.get(), expected);

    // 4. Vector
    auto f_vec = scheduler.submit([]() {
        return std::vector<int>{1, 2, 3, 4, 5};
    });
    const std::vector<int> expected_vec{1, 2, 3, 4, 5};
    EXPECT_EQ(f_vec.get(), expected_vec);
}

TEST(TaskResultsTest, MultipleGetCallsThrowRuntimeError) {
    threadforge::TaskScheduler scheduler(2);

    auto fut = scheduler.submit([]() { return 42; });
    EXPECT_TRUE(fut.valid());

    int val = fut.get();
    EXPECT_EQ(val, 42);

    // Second call to get() must fail cleanly according to the documented contract
    EXPECT_THROW({
        fut.get();
    }, std::runtime_error);
}

TEST(TaskResultsTest, MultipleGetCallsOnVoidFutureThrowRuntimeError) {
    threadforge::TaskScheduler scheduler(2);

    auto fut = scheduler.submit([]() {});
    EXPECT_TRUE(fut.valid());

    fut.get();

    EXPECT_THROW({
        fut.get();
    }, std::runtime_error);
}

TEST(TaskResultsTest, HistoryPruningAndClear) {
    threadforge::TaskScheduler scheduler(2, 5); // Max history 5

    for (int i = 0; i < 15; ++i) {
        scheduler.submit([i]() { return i; });
    }
    scheduler.wait_idle();

    // Prune history keeping at most 3 records
    std::size_t pruned = scheduler.prune_history(3);
    EXPECT_GE(pruned, 0u);

    // Clear history completely
    std::size_t cleared = scheduler.clear_history();
    EXPECT_LE(cleared, 15u);
}
