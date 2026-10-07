#include "znet/internal/connection_actor.h"

#include <atomic>
#include <cerrno>
#include <gtest/gtest.h>
#include <vector>

#include "zco/sched.h"
#include "znet/znet_logger.h"

namespace znet {
namespace detail {
namespace {

TEST(ConnectionActorTest, ReentrantCommandUsesCurrentWorkerAndKeepsError) {
    zco::init(1);
    std::vector<ConnectionEventType> processed;
    std::atomic<int> retained{0};
    auto owner = std::make_shared<int>(42);
    ConnectionActor *active_actor = nullptr;
    ConnectionActor actor(
        zco::main_sched(),
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
    zco::shutdown();
    EXPECT_EQ(retained.load(), 1);
    EXPECT_EQ(processed,
              (std::vector<ConnectionEventType>{ConnectionEventType::kRead,
                                                ConnectionEventType::kFlush}));
}

} // 命名空间
} // 命名空间 detail
} // 命名空间 znet

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    znet::init_logger();
    return RUN_ALL_TESTS();
}
