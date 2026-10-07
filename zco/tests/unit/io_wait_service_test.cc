#include "zco/internal/io_wait_service.h"

#include <cerrno>
#include <gtest/gtest.h>
#include <sys/epoll.h>

#include "zco/internal/poller.h"
#include "zco/internal/processor.h"
#include "zco/zco_logger.h"

namespace zco {
namespace {

class RecordingPoller : public Poller {
  public:
    bool start() override { return true; }
    void stop() override {}
    void wake() override {}
    bool register_waiter(const std::shared_ptr<IoWaiter> &waiter) override {
        if (fail_registration) {
            errno = EBADF;
            return false;
        }
        registered = waiter;
        return true;
    }
    void unregister_waiter(const std::shared_ptr<IoWaiter> &) override {
        ++unregister_count;
    }
    std::vector<std::shared_ptr<IoWaiter>> cancel_fd(int fd,
                                                     int error) override {
        if (!registered || registered->fd != fd) {
            return {};
        }
        registered->error.store(error);
        return {registered};
    }
    void wait_events(int,
                     const std::function<void(const std::shared_ptr<IoWaiter> &,
                                              uint32_t)> &) override {}

    bool fail_registration = false;
    int unregister_count = 0;
    std::shared_ptr<IoWaiter> registered;
};

class IoWaitServiceTest : public ::testing::Test {
  protected:
    Processor owner_{50, 64 * 1024};
    Fiber::ptr fiber_ =
        std::make_shared<Fiber>(1, &owner_, [] {}, 64 * 1024, 0, true);
    TimerQueue timers_;
    RecordingPoller *poller_ = new RecordingPoller;
    int resumes_ = 0;
    IoWaitService service_{owner_.id(), timers_,
                           std::unique_ptr<Poller>(poller_),
                           [this](const Fiber::ptr &fiber, bool timed_out) {
                               ++resumes_;
                               EXPECT_TRUE(fiber->try_wake(timed_out));
                           }};
};

TEST_F(IoWaitServiceTest, ReadinessWinsOverDueTimeoutAndDuplicateReadiness) {
    EXPECT_TRUE(service_.wait(fiber_, 7, EPOLLIN, 0, [this] {
        service_.handle_ready(poller_->registered, EPOLLIN);
        timers_.process_due();
        service_.handle_ready(poller_->registered, EPOLLIN);
        return !fiber_->timed_out();
    }));
    EXPECT_EQ(resumes_, 1);
    EXPECT_FALSE(fiber_->timed_out());
    EXPECT_TRUE(poller_->registered->timer->cancelled.load());
    EXPECT_FALSE(poller_->registered->active.load());
    EXPECT_GE(poller_->unregister_count, 1);
}

TEST_F(IoWaitServiceTest, TimeoutWinsOverLateReadiness) {
    EXPECT_FALSE(service_.wait(fiber_, 7, EPOLLIN, 0, [this] {
        timers_.process_due();
        service_.handle_ready(poller_->registered, EPOLLIN);
        return !fiber_->timed_out();
    }));
    EXPECT_EQ(resumes_, 1);
    EXPECT_TRUE(fiber_->timed_out());
    EXPECT_FALSE(poller_->registered->active.load());
    EXPECT_GE(poller_->unregister_count, 1);
}

TEST_F(IoWaitServiceTest,
       CancellationPropagatesErrorAndSuppressesOtherWakeups) {
    EXPECT_FALSE(service_.wait(fiber_, 7, EPOLLIN, 0, [this] {
        service_.cancel(7, EBADF);
        service_.handle_ready(poller_->registered, EPOLLIN);
        timers_.process_due();
        return true;
    }));
    EXPECT_EQ(errno, EBADF);
    EXPECT_EQ(resumes_, 1);
    EXPECT_FALSE(fiber_->timed_out());
    EXPECT_TRUE(poller_->registered->timer->cancelled.load());
}

TEST_F(IoWaitServiceTest,
       RegistrationFailureRestoresRunningStateWithoutParking) {
    poller_->fail_registration = true;
    fiber_->mark_running();
    bool parked = false;
    EXPECT_FALSE(service_.wait(fiber_, 7, EPOLLIN, 0, [&parked] {
        parked = true;
        return true;
    }));
    EXPECT_EQ(errno, EBADF);
    EXPECT_EQ(fiber_->state(), Fiber::State::kRunning);
    EXPECT_FALSE(parked);
    EXPECT_EQ(resumes_, 0);
}

} // 命名空间
} // 命名空间 zco

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    zco::init_logger();
    return RUN_ALL_TESTS();
}
