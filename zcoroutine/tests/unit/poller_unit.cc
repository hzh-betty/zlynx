#include <errno.h>

#include <gtest/gtest.h>

#include "zcoroutine/internal/poller.h"
#include "support/test_fixture.h"

namespace zcoroutine {
namespace {

class PollerUnitTest : public test::RuntimeTestBase {};

TEST_F(PollerUnitTest, DefaultPollerLifecycleAndInvalidRegistration) {
  std::unique_ptr<Poller> poller = create_default_poller();
  ASSERT_NE(poller, nullptr);
  ASSERT_TRUE(poller->start());

  poller->wake();
  poller->wait_events(1, [](const std::shared_ptr<IoWaiter>& waiter, uint32_t events) {
    (void)waiter;
    (void)events;
  });

  poller->stop();

  std::shared_ptr<IoWaiter> waiter = std::make_shared<IoWaiter>();
  waiter->fd = -1;
  waiter->events = 0;
  waiter->active.store(true, std::memory_order_release);

  errno = 0;
  EXPECT_FALSE(poller->register_waiter(waiter));
  EXPECT_EQ(errno, EINVAL);
}

}  // namespace
}  // namespace zcoroutine
