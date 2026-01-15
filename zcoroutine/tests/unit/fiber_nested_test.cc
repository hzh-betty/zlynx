#include "runtime/fiber.h"
#include "runtime/shared_stack.h"
#include "util/thread_context.h"
#include "util/zcoroutine_logger.h"
#include <gtest/gtest.h>
#include <vector>

using namespace zcoroutine;

class FiberNestedFixture : public ::testing::Test {
protected:
  void SetUp() override {
    zcoroutine::init_logger(zlog::LogLevel::value::DEBUG);
    ThreadContext::reset_shared_stack_config();
  }
  void TearDown() override { ThreadContext::reset_shared_stack_config(); }
};

TEST_F(FiberNestedFixture, NestedYieldReturnsToCaller) {
  std::vector<int> steps;

  Fiber::ptr child;
  auto parent = std::make_shared<Fiber>([&]() {
    steps.push_back(1);
    child = std::make_shared<Fiber>([&]() {
      steps.push_back(2);
      Fiber::yield();
      steps.push_back(3);
    });
    child->resume();
    steps.push_back(4);
    child->resume();
    steps.push_back(5);
  });

  parent->resume();
  EXPECT_EQ(steps, (std::vector<int>{1, 2, 4, 3, 5}));
  EXPECT_EQ(parent->state(), Fiber::State::kTerminated);
  EXPECT_EQ(child->state(), Fiber::State::kTerminated);
}

TEST_F(FiberNestedFixture, ThreeLevelNested) {
  std::vector<int> steps;

  Fiber::ptr b, c;
  auto a = std::make_shared<Fiber>([&]() {
    steps.push_back(1);
    b = std::make_shared<Fiber>([&]() {
      steps.push_back(2);
      c = std::make_shared<Fiber>([&]() {
        steps.push_back(3);
        Fiber::yield();
      });
      c->resume();
      steps.push_back(5);
      Fiber::yield();
    });
    b->resume();
    steps.push_back(7);
  });

  a->resume();
  EXPECT_EQ(steps, (std::vector<int>{1, 2, 3, 5, 7}));
  EXPECT_EQ(a->state(), Fiber::State::kTerminated);
  EXPECT_EQ(b->state(), Fiber::State::kSuspended);
  EXPECT_EQ(c->state(), Fiber::State::kSuspended);
}

TEST_F(FiberNestedFixture, DeepNestedModerateDepth) {
  const int limit = 10;
  std::atomic<int> max_depth{0};
  std::function<void(int)> make;
  make = [&](int d) {
    if (d > limit)
      return;
    auto f = std::make_shared<Fiber>([&, d]() {
      int size = ThreadContext::call_stack_size();
      if (size > max_depth.load())
        max_depth.store(size);
      if (d < limit) {
        make(d + 1);
      }
      Fiber::yield();
    });
    f->resume();
  };
  auto root = std::make_shared<Fiber>([&]() { make(1); });
  root->resume();
  EXPECT_GE(max_depth.load(), 3);
  EXPECT_LE(max_depth.load(), ThreadContext::kMaxCallStackDepth);
}

TEST_F(FiberNestedFixture, DeepNested64Levels) {
  const int limit = 64;
  std::atomic<int> max_depth{0};
  std::function<void(int)> make;
  make = [&](int d) {
    if (d > limit)
      return;
    auto f = std::make_shared<Fiber>([&, d]() {
      int size = ThreadContext::call_stack_size();
      if (size > max_depth.load())
        max_depth.store(size);
      if (d < limit) {
        make(d + 1);
      }
      Fiber::yield();
    });
    f->resume();
  };
  auto root = std::make_shared<Fiber>([&]() { make(1); });
  root->resume();
  EXPECT_GE(max_depth.load(), 32);
  EXPECT_LE(max_depth.load(), ThreadContext::kMaxCallStackDepth);
}

TEST_F(FiberNestedFixture, DeepNestedBeyondLimitWarnsAndCapsDepth) {
  const int limit = ThreadContext::kMaxCallStackDepth + 8;
  std::atomic<int> max_depth{0};
  std::function<void(int)> make;
  make = [&](int d) {
    if (d > limit)
      return;
    auto f = std::make_shared<Fiber>([&, d]() {
      int size = ThreadContext::call_stack_size();
      if (size > max_depth.load())
        max_depth.store(size);
      if (d < limit) {
        make(d + 1);
      }
      Fiber::yield();
    });
    f->resume();
  };
  auto root = std::make_shared<Fiber>([&]() { make(1); });
  root->resume();
  EXPECT_EQ(max_depth.load(), ThreadContext::kMaxCallStackDepth);
}

TEST_F(FiberNestedFixture, NestedExceptionPropagation) {
  bool caught = false;
  auto parent = std::make_shared<Fiber>([&]() {
    auto child = std::make_shared<Fiber>([]() {
      Fiber::yield();
      throw std::runtime_error("child error");
    });
    try {
      child->resume();
      child->resume();
    } catch (const std::runtime_error &) {
      caught = true;
    }
  });
  parent->resume();
  EXPECT_TRUE(caught);
  EXPECT_EQ(parent->state(), Fiber::State::kTerminated);
}

TEST_F(FiberNestedFixture, NestedWithSharedStack) {
  ThreadContext::set_stack_mode(StackMode::kShared);
  std::vector<int> steps;
  Fiber::ptr child;
  auto parent = std::make_shared<Fiber>([&]() {
    steps.push_back(1);
    child = std::make_shared<Fiber>([&]() {
      steps.push_back(2);
      Fiber::yield();
      steps.push_back(3);
    });
    child->resume();
    steps.push_back(4);
    child->resume();
    steps.push_back(5);
  });
  parent->resume();
  EXPECT_EQ(steps, (std::vector<int>{1, 2, 4, 3, 5}));
  EXPECT_EQ(parent->state(), Fiber::State::kTerminated);
  EXPECT_EQ(child->state(), Fiber::State::kTerminated);
  // teardown resets shared stack
}

TEST_F(FiberNestedFixture, ParentResumesChildMultipleTimesSafely) {
  int count = 0;
  Fiber::ptr child;
  auto parent = std::make_shared<Fiber>([&]() {
    child = std::make_shared<Fiber>([&]() {
      count++;
      Fiber::yield();
      count++;
    });
    child->resume();
    child->resume();
  });
  parent->resume();
  EXPECT_EQ(count, 2);
  EXPECT_EQ(parent->state(), Fiber::State::kTerminated);
  EXPECT_EQ(child->state(), Fiber::State::kTerminated);
}

int main(int argc, char **argv) {
  zcoroutine::init_logger(zlog::LogLevel::value::DEBUG);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
