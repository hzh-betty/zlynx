#include "execution/stack_arena.h"
#include "support/runtime_fixture.h"
#include <cerrno>
using namespace zco;

namespace {
void nested(int depth, int seed) {
    volatile int values[64];
    for (int i = 0; i < 64; ++i)
        values[i] = seed + i;
    if (depth)
        nested(depth - 1, seed + 100);
    else
        for (int i = 0; i < 10; ++i)
            yield();
    for (int i = 0; i < 64; ++i)
        EXPECT_EQ(values[i], seed + i);
}

TEST(Execution, SharedSlotAndIndependentStacksPreserveNestedCallsAndErrno) {
    for (auto model : {StackModel::kShared, StackModel::kIndependent}) {
        RuntimeOptions options{1};
        options.shared_stack_count = 1;
        options.stack_model = model;
        Runtime runtime(options);
        std::vector<TaskHandle> tasks;
        for (int i = 0; i < 30; ++i)
            tasks.push_back(test::spawn(runtime, [i] {
                int expected = i % 2 ? EDOM : EINVAL;
                errno = expected;
                for (int n = 0; n < 10; ++n) {
                    yield();
                    EXPECT_EQ(errno, expected);
                    nested(3, i * 1000);
                    EXPECT_TRUE(sleep_for(std::chrono::microseconds(50)));
                    EXPECT_EQ(errno, expected);
                }
            }));
        for (auto &task : tasks)
            EXPECT_TRUE(task.join(test::soon()));
    }
}

TEST(Execution, InvalidStackSizeFailsExplicitly) {
    EXPECT_THROW(detail::StackBuffer stack(1), std::invalid_argument);
}
} // namespace

TEST(Execution, IndependentCacheReusesOnlyCompletedBuffersAndIsBounded) {
    RuntimeOptions options{1};
    options.stack_model = StackModel::kIndependent;
    detail::StackArena arena(options);
    auto first = arena.acquire_independent();
    auto address = first->data();
    auto second = arena.acquire_independent();
    EXPECT_NE(first->data(), second->data());
    arena.release_independent(std::move(first));
    arena.release_independent(std::move(second));
    auto reused = arena.acquire_independent();
    EXPECT_EQ(reused->data(), address);
}
