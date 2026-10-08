#include "znet/internal/connection_actor.h"

#include <atomic>
#include <cerrno>
#include <future>
#include <gtest/gtest.h>
#include <vector>

#include "zco/coroutine.h"
#include "znet/znet_logger.h"

namespace znet {
namespace detail {
namespace {

TEST(ConnectionActorTest, ReentrantCommandUsesCurrentWorkerAndKeepsError) {
    zco::Runtime runtime(zco::RuntimeOptions{1});
    std::vector<ConnectionEventType> processed;
    std::atomic<int> retained{0};
    auto owner = std::make_shared<int>(42);
    ConnectionActor *active_actor = nullptr;
    ConnectionActor actor(
        runtime.executor(0),
        [&](const ConnectionActor::EventPtr &event) {
            processed.push_back(event->type);
            if (event->type == ConnectionEventType::kRead) {
                auto nested = std::make_shared<ConnectionEvent>(
                    ConnectionEventType::kFlush);
                EXPECT_EQ(active_actor->dispatch(nested), -1);
                EXPECT_EQ(errno, EPIPE);
                event->result = 12;
            } else {
                event->result = -1;
                event->error = EPIPE;
            }
        },
        [&]() -> std::shared_ptr<void> {
            ++retained;
            return owner;
        });
    active_actor = &actor;

    EXPECT_EQ(actor.dispatch(std::make_shared<ConnectionEvent>(
                  ConnectionEventType::kRead)),
              12);
    // 检查状态或销毁命令调度器前，先等待工作协程结束。
    runtime.request_stop();
    runtime.join();
    EXPECT_EQ(retained.load(), 1);
    EXPECT_EQ(processed,
              (std::vector<ConnectionEventType>{ConnectionEventType::kRead,
                                                ConnectionEventType::kFlush}));
}

TEST(ConnectionActorTest, CanceledQueuedTaskReleasesThreadWaiter) {
    zco::Runtime runtime(zco::RuntimeOptions{1});
    std::promise<void> entered, release, queued;
    auto gate = release.get_future().share();
    auto blocker = runtime.executor(0).spawn([&] {
        entered.set_value();
        gate.wait();
    });
    entered.get_future().wait();
    auto owner = std::make_shared<int>(1);
    ConnectionActor actor(
        runtime.executor(0), [](const ConnectionActor::EventPtr &) { FAIL(); },
        [&]() -> std::shared_ptr<void> {
            queued.set_value();
            return owner;
        });
    std::thread caller([&] {
        EXPECT_EQ(actor.dispatch(std::make_shared<ConnectionEvent>(
                      ConnectionEventType::kRead)),
                  -1);
        EXPECT_EQ(errno, ECANCELED);
    });
    queued.get_future().wait();
    runtime.request_stop();
    release.set_value();
    caller.join();
    runtime.join();
}

TEST(ConnectionActorTest, HandlerExceptionIsObservedByDispatcher) {
    zco::Runtime runtime(zco::RuntimeOptions{1});
    auto owner = std::make_shared<int>(1);
    ConnectionActor actor(
        runtime.executor(0),
        [](const ConnectionActor::EventPtr &) {
            throw std::runtime_error("handler failure");
        },
        [&]() -> std::shared_ptr<void> { return owner; });
    EXPECT_THROW(actor.dispatch(std::make_shared<ConnectionEvent>(
                     ConnectionEventType::kRead)),
                 std::runtime_error);
}

} // 命名空间
} // 命名空间 detail
} // 命名空间 znet

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    znet::init_logger();
    return RUN_ALL_TESTS();
}
