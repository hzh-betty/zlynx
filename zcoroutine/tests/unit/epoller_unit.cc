#include <errno.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>

#include <gtest/gtest.h>

#include "zcoroutine/internal/epoller.h"
#include "support/test_fixture.h"

namespace zcoroutine {
namespace {

class EpollerUnitTest : public test::RuntimeTestBase {};

TEST_F(EpollerUnitTest, StartWakeStopFlowWorks) {
  Epoller epoller;
  ASSERT_TRUE(epoller.start());

  epoller.wake();
  int callback_count = 0;
  epoller.wait_events(5,
                      [&callback_count](const std::shared_ptr<IoWaiter>& waiter, uint32_t events) {
                        (void)waiter;
                        (void)events;
                        ++callback_count;
                      });

  EXPECT_EQ(callback_count, 0);
  epoller.stop();
  epoller.stop();
}

TEST_F(EpollerUnitTest, RegisterWaiterValidationAndConflict) {
  Epoller epoller;
  ASSERT_TRUE(epoller.start());

  errno = 0;
  EXPECT_FALSE(epoller.register_waiter(nullptr));
  EXPECT_EQ(errno, EINVAL);

  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

  std::shared_ptr<IoWaiter> invalid_events = std::make_shared<IoWaiter>();
  invalid_events->fd = pair[0];
  invalid_events->events = 0;
  invalid_events->active.store(true, std::memory_order_release);
  errno = 0;
  EXPECT_FALSE(epoller.register_waiter(invalid_events));
  EXPECT_EQ(errno, EINVAL);

  std::shared_ptr<IoWaiter> read1 = std::make_shared<IoWaiter>();
  read1->fd = pair[0];
  read1->events = EPOLLIN;
  read1->active.store(true, std::memory_order_release);
  EXPECT_TRUE(epoller.register_waiter(read1));

  std::shared_ptr<IoWaiter> read2 = std::make_shared<IoWaiter>();
  read2->fd = pair[0];
  read2->events = EPOLLIN;
  read2->active.store(true, std::memory_order_release);
  errno = 0;
  EXPECT_FALSE(epoller.register_waiter(read2));
  EXPECT_EQ(errno, EBUSY);

  epoller.unregister_waiter(read1);
  EXPECT_TRUE(epoller.register_waiter(read2));

  epoller.unregister_waiter(read2);
  ::close(pair[0]);
  ::close(pair[1]);
  epoller.stop();
}

TEST_F(EpollerUnitTest, WaitEventsDispatchesReadyWaiter) {
  Epoller epoller;
  ASSERT_TRUE(epoller.start());

  int pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);

  std::shared_ptr<IoWaiter> waiter = std::make_shared<IoWaiter>();
  waiter->fd = pair[1];
  waiter->events = EPOLLIN;
  waiter->active.store(true, std::memory_order_release);

  ASSERT_TRUE(epoller.register_waiter(waiter));

  const char marker = 'a';
  ASSERT_EQ(::write(pair[0], &marker, 1), 1);

  int callback_count = 0;
  epoller.wait_events(
      100, [&callback_count, waiter](const std::shared_ptr<IoWaiter>& ready, uint32_t events) {
        ++callback_count;
        EXPECT_EQ(ready.get(), waiter.get());
        EXPECT_NE(events & (EPOLLIN | EPOLLHUP | EPOLLERR), 0u);
      });

  EXPECT_EQ(callback_count, 1);

  ::close(pair[0]);
  ::close(pair[1]);
  epoller.stop();
}

}  // namespace
}  // namespace zcoroutine
