#include "runtime/task_queues.h"
#include "runtime/worker.h"
#include "support/fake_reactor.h"
#include "support/runtime_fixture.h"
#include <ucontext.h>
using namespace zco;

namespace {
thread_local std::function<void()> *switch_hook = nullptr;
std::atomic<bool> fail_context{false};
thread_local bool fail_snapshot_allocation = false;
} // namespace

extern "C" int __real_getcontext(ucontext_t *);

extern "C" int __wrap_getcontext(ucontext_t *context) {
    if (fail_context.exchange(false)) {
        errno = EIO;
        return -1;
    }
    return __real_getcontext(context);
}

extern "C" void *__real__Znwm(size_t);

extern "C" void *__wrap__Znwm(size_t size) {
    if (fail_snapshot_allocation) {
        fail_snapshot_allocation = false;
        throw std::bad_alloc();
    }
    return __real__Znwm(size);
}

extern "C" int __real_swapcontext(ucontext_t *, const ucontext_t *);

extern "C" int __wrap_swapcontext(ucontext_t *from, const ucontext_t *to) {
    auto *hook = switch_hook;
    switch_hook = nullptr;
    if (hook)
        (*hook)();
    return __real_swapcontext(from, to);
}

TEST(Scheduling, PinnedQueueCannotBeStolen) {
    detail::TaskQueues queues;
    auto first = std::make_shared<detail::Completion>(TaskId{1});
    auto second = std::make_shared<detail::Completion>(TaskId{2});
    queues.push({[] {}, first}, true);
    queues.push({[] {}, second}, false);
    detail::PendingTask task;
    EXPECT_TRUE(queues.steal(task));
    EXPECT_EQ(task.completion->id, second->id);
    EXPECT_FALSE(queues.steal(task));
    EXPECT_TRUE(queues.take(task));
    EXPECT_EQ(task.completion->id, first->id);
}

TEST(Scheduling, CompleteBeforeParkAndDuringSwitchEnqueuesOnce) {
    Runtime runtime(RuntimeOptions{1});
    std::atomic<int> executions{0};
    auto task = test::spawn(runtime, [&] {
        for (int i = 0; i < 300; ++i) {
            detail::WaitTicket ticket;
            auto token = ticket.token();
            std::function<void()> hook = [&] { EXPECT_TRUE(token.complete()); };
            if (i % 2)
                switch_hook = &hook;
            else
                EXPECT_TRUE(token.complete());
            EXPECT_EQ(ticket.wait(test::soon()), WaitOutcome::ready);
            EXPECT_FALSE(token.complete());
            ++executions;
            yield();
        }
    });
    EXPECT_TRUE(task.join(test::soon()));
    EXPECT_EQ(executions, 300);
    EXPECT_TRUE(test::spawn(runtime, [&] { ++executions; }).join(test::soon()));
    EXPECT_EQ(executions, 301);
}

TEST(Scheduling, PinnedTasksAndCreatedContextsRemainOnTheirWorker) {
    Runtime runtime(RuntimeOptions{3});
    std::vector<TaskHandle> tasks;
    for (size_t worker = 0; worker < 3; ++worker)
        for (int n = 0; n < 20; ++n)
            tasks.push_back(test::spawn(runtime.executor(worker), [worker] {
                auto identity = current_task_id();
                for (int i = 0; i < 20; ++i) {
                    EXPECT_EQ(current_executor().index(), worker);
                    EXPECT_EQ(current_task_id(), identity);
                    yield();
                }
            }));
    for (auto &task : tasks)
        EXPECT_TRUE(task.join(test::soon()));
}

TEST(Scheduling, YieldAllowsNewTasksToRun) {
    Runtime runtime(RuntimeOptions{1});
    std::atomic<bool> finished{false};
    auto first = test::spawn(runtime, [&] {
        while (!finished)
            yield();
    });
    auto second = test::spawn(runtime, [&] { finished = true; });
    EXPECT_TRUE(second.join(test::soon()));
    EXPECT_TRUE(first.join(test::soon()));
}

TEST(Scheduling, ContextCreationFailureIsObservedAndRuntimeStaysUsable) {
    Runtime runtime(RuntimeOptions{1});
    fail_context = true;
    auto failed = test::spawn(runtime, [] { FAIL() << "Invalid context ran"; });
    EXPECT_THROW(failed.join(test::soon()), std::system_error);
    EXPECT_EQ(failed.status(), TaskStatus::failed);
    EXPECT_TRUE(test::spawn(runtime, [] {}).join(test::soon()));
}

TEST(Scheduling, SnapshotAllocationFailureUnwindsTheActiveStack) {
    Runtime runtime(RuntimeOptions{1});
    std::atomic<int> destroyed{0};
    auto failed = test::spawn(runtime, [&] {
        struct Guard {
            std::atomic<int> &count;
            ~Guard() { ++count; }
        } guard{destroyed};
        std::function<void()> hook = [] { fail_snapshot_allocation = true; };
        switch_hook = &hook;
        yield();
    });
    EXPECT_THROW(failed.join(test::soon()), std::bad_alloc);
    EXPECT_EQ(destroyed, 1);
    EXPECT_TRUE(test::spawn(runtime, [] {}).join(test::soon()));
}

TEST(Scheduling, WorkerStealsOrdinaryTasksBeforeContextCreation) {
    auto endpoint = std::make_shared<detail::Submission>();
    auto first_reactor = std::make_shared<test::FakeReactor>();
    auto second_reactor = std::make_shared<test::FakeReactor>();
    detail::Worker first(0, RuntimeOptions{2}, endpoint, first_reactor);
    detail::Worker second(1, RuntimeOptions{2}, endpoint, second_reactor);
    endpoint->workers = {&first, &second};
    first.set_peers(endpoint->workers);
    second.set_peers(endpoint->workers);
    auto startup = std::make_shared<detail::Startup>();
    first.start(startup);
    second.start(startup);
    {
        std::unique_lock<std::mutex> lock(startup->mutex);
        startup->cv.wait(lock, [&] { return startup->ready == 2; });
        endpoint->accepting = true;
        startup->launch = true;
        startup->cv.notify_all();
    }
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    auto blocker = detail::submit(
        endpoint,
        [&] {
            entered.set_value();
            gate.wait();
        },
        0, true);
    entered.get_future().wait();
    auto completion = std::make_shared<detail::Completion>(TaskId{900});
    first.push({[] { EXPECT_EQ(current_executor().index(), 1u); }, completion},
               false);
    second.wake();
    EXPECT_TRUE(TaskHandle(completion).join(test::soon()));
    {
        std::lock_guard<std::mutex> lock(endpoint->mutex);
        endpoint->accepting = false;
    }
    release.set_value();
    first.stop();
    second.stop();
    first.join();
    second.join();
}
