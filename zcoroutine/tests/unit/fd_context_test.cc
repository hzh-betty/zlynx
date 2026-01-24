#include "channel.h"
#include "fiber.h"
#include "zcoroutine_logger.h"
#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace zcoroutine;

class FdContextTest : public ::testing::Test {
protected:
  void SetUp() override {
    // 创建一对socket用于测试
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    fd1_ = fds[0];
    fd2_ = fds[1];

    // 设置非阻塞
    fcntl(fd1_, F_SETFL, O_NONBLOCK);
    fcntl(fd2_, F_SETFL, O_NONBLOCK);

    ctx1_ = std::make_shared<Channel>(fd1_);
    ctx2_ = std::make_shared<Channel>(fd2_);
  }

  void TearDown() override {
    ctx1_.reset();
    ctx2_.reset();
    if (fd1_ >= 0)
      close(fd1_);
    if (fd2_ >= 0)
      close(fd2_);
  }

  int fd1_ = -1;
  int fd2_ = -1;
  Channel::ptr ctx1_;
  Channel::ptr ctx2_;
};

// 测试1：创建FdContext
TEST_F(FdContextTest, CreateFdContext) {
  ASSERT_NE(ctx1_, nullptr);
  EXPECT_EQ(ctx1_->fd(), fd1_);
  EXPECT_EQ(ctx1_->events(), Channel::kNone);
}

// 测试2：添加读事件
TEST_F(FdContextTest, AddReadEvent) {
  int result = ctx1_->add_event(Channel::kRead);
  EXPECT_EQ(result, Channel::kRead);
  EXPECT_EQ(ctx1_->events(), Channel::kRead);
}

// 测试3：添加写事件
TEST_F(FdContextTest, AddWriteEvent) {
  int result = ctx1_->add_event(Channel::kWrite);
  EXPECT_EQ(result, Channel::kWrite);
  EXPECT_EQ(ctx1_->events(), Channel::kWrite);
}

// 测试4：添加读写事件
TEST_F(FdContextTest, AddReadWriteEvents) {
  ctx1_->add_event(Channel::kRead);
  ctx1_->add_event(Channel::kWrite);

  EXPECT_EQ(ctx1_->events(), Channel::kRead | Channel::kWrite);
}

// 测试5：删除读事件
TEST_F(FdContextTest, DelReadEvent) {
  ctx1_->add_event(Channel::kRead);
  ctx1_->add_event(Channel::kWrite);

  int result = ctx1_->del_event(Channel::kRead);
  EXPECT_EQ(result, Channel::kWrite);
  EXPECT_EQ(ctx1_->events(), Channel::kWrite);
}

// 测试6：删除写事件
TEST_F(FdContextTest, DelWriteEvent) {
  ctx1_->add_event(Channel::kRead);
  ctx1_->add_event(Channel::kWrite);

  int result = ctx1_->del_event(Channel::kWrite);
  EXPECT_EQ(result, Channel::kRead);
  EXPECT_EQ(ctx1_->events(), Channel::kRead);
}

// 测试7：删除所有事件
TEST_F(FdContextTest, DelAllEvents) {
  ctx1_->add_event(Channel::kRead);
  ctx1_->add_event(Channel::kWrite);

  ctx1_->del_event(Channel::kRead);
  ctx1_->del_event(Channel::kWrite);

  EXPECT_EQ(ctx1_->events(), Channel::kNone);
}

// 测试8：取消读事件
TEST_F(FdContextTest, CancelReadEvent) {
  bool callback_called = false;

  ctx1_->add_event(Channel::kRead);
  auto &read_ctx = ctx1_->get_event_context(Channel::kRead);
  read_ctx.callback = [&callback_called]() { callback_called = true; };

  ctx1_->cancel_event(Channel::kRead);

  EXPECT_EQ(ctx1_->events(), Channel::kNone);
}

// 测试9：取消所有事件
TEST_F(FdContextTest, CancelAllEvents) {
  ctx1_->add_event(Channel::kRead);
  ctx1_->add_event(Channel::kWrite);

  ctx1_->cancel_all();

  EXPECT_EQ(ctx1_->events(), Channel::kNone);
}

// ==================== 事件上下文测试 ====================

// 测试10：获取读事件上下文
TEST_F(FdContextTest, GetReadEventContext) {
  ctx1_->add_event(Channel::kRead);
  auto &read_ctx = ctx1_->get_event_context(Channel::kRead);

  // 设置回调
  bool called = false;
  read_ctx.callback = [&called]() { called = true; };

  ASSERT_NE(read_ctx.callback, nullptr);
  read_ctx.callback();
  EXPECT_TRUE(called);
}

// 测试11：获取写事件上下文
TEST_F(FdContextTest, GetWriteEventContext) {
  ctx1_->add_event(Channel::kWrite);
  auto &write_ctx = ctx1_->get_event_context(Channel::kWrite);

  // 设置协程
  auto fiber = std::make_shared<Fiber>([]() {});
  write_ctx.fiber = fiber;

  ASSERT_NE(write_ctx.fiber, nullptr);
  EXPECT_EQ(write_ctx.fiber, fiber);
}

// 测试12：重置事件上下文
TEST_F(FdContextTest, ResetEventContext) {
  ctx1_->add_event(Channel::kRead);
  auto &read_ctx = ctx1_->get_event_context(Channel::kRead);

  read_ctx.callback = []() {};
  read_ctx.fiber = std::make_shared<Fiber>([]() {});

  ctx1_->reset_event_context(read_ctx);

  EXPECT_EQ(read_ctx.callback, nullptr);
  EXPECT_EQ(read_ctx.fiber, nullptr);
}

// 测试13：触发读事件回调
TEST_F(FdContextTest, TriggerReadEventCallback) {
  bool callback_called = false;

  ctx1_->add_event(Channel::kRead);
  auto &read_ctx = ctx1_->get_event_context(Channel::kRead);
  read_ctx.callback = [&callback_called]() { callback_called = true; };

  ctx1_->trigger_event(Channel::kRead);

  EXPECT_TRUE(callback_called);
}

// 测试14：触发写事件回调
TEST_F(FdContextTest, TriggerWriteEventCallback) {
  int value = 0;

  ctx1_->add_event(Channel::kWrite);
  auto &write_ctx = ctx1_->get_event_context(Channel::kWrite);
  write_ctx.callback = [&value]() { value = 42; };

  ctx1_->trigger_event(Channel::kWrite);

  EXPECT_EQ(value, 42);
}

// 测试15：触发事件恢复协程
TEST_F(FdContextTest, TriggerEventResumesFiber) {
  bool fiber_executed = false;

  ctx1_->add_event(Channel::kRead);
  auto &read_ctx = ctx1_->get_event_context(Channel::kRead);

  auto fiber =
      std::make_shared<Fiber>([&fiber_executed]() { fiber_executed = true; });
  read_ctx.fiber = fiber;

  ctx1_->trigger_event(Channel::kRead);

  // 注意：trigger_event 只是准备恢复，实际需要调度器来执行
  // 这里我们手动执行协程来验证
  if (fiber->state() == Fiber::State::kReady) {
    fiber->resume();
  }

  EXPECT_TRUE(fiber_executed);
}

// ==================== 边界条件测试 ====================

// 测试16：重复添加相同事件
TEST_F(FdContextTest, AddSameEventTwice) {
  ctx1_->add_event(Channel::kRead);

  // 第二次添加应该被忽略或返回错误
  int result = ctx1_->add_event(Channel::kRead);
  EXPECT_EQ(result, Channel::kRead);

  // 事件应该保持不变
  EXPECT_EQ(ctx1_->events() & Channel::kRead, Channel::kRead);
}

// 测试17：删除不存在的事件
TEST_F(FdContextTest, DelNonexistentEvent) {
  EXPECT_EQ(ctx1_->events(), Channel::kNone);

  int result = ctx1_->del_event(Channel::kRead);
  EXPECT_EQ(result, Channel::kNone);

  EXPECT_EQ(ctx1_->events(), Channel::kNone);
}

// 测试18：取消不存在的事件
TEST_F(FdContextTest, CancelNonexistentEvent) {
  EXPECT_NO_THROW({ ctx1_->cancel_event(Channel::kRead); });
}

// 测试19：触发未设置的事件
TEST_F(FdContextTest, TriggerUnsetEvent) {
  EXPECT_NO_THROW({ ctx1_->trigger_event(Channel::kRead); });
}

// 测试20：无效文件描述符
TEST_F(FdContextTest, InvalidFileDescriptor) {
  auto invalid_ctx = std::make_shared<Channel>(-1);
  EXPECT_EQ(invalid_ctx->fd(), -1);
  EXPECT_EQ(invalid_ctx->events(), Channel::kNone);
}

// ==================== 多事件组合测试 ====================

// 测试21：同时设置读写回调
TEST_F(FdContextTest, BothReadWriteCallbacks) {
  bool read_called = false;
  bool write_called = false;

  ctx1_->add_event(Channel::kRead);
  ctx1_->add_event(Channel::kWrite);

  auto &read_ctx = ctx1_->get_event_context(Channel::kRead);
  auto &write_ctx = ctx1_->get_event_context(Channel::kWrite);

  read_ctx.callback = [&read_called]() { read_called = true; };
  write_ctx.callback = [&write_called]() { write_called = true; };

  ctx1_->trigger_event(Channel::kRead);
  ctx1_->trigger_event(Channel::kWrite);

  EXPECT_TRUE(read_called);
  EXPECT_TRUE(write_called);
}

// 测试22：读写事件独立管理
TEST_F(FdContextTest, IndependentReadWriteManagement) {
  ctx1_->add_event(Channel::kRead);
  ctx1_->add_event(Channel::kWrite);

  auto &read_ctx = ctx1_->get_event_context(Channel::kRead);
  auto &write_ctx = ctx1_->get_event_context(Channel::kWrite);

  read_ctx.callback = []() {};
  write_ctx.fiber = std::make_shared<Fiber>([]() {});

  EXPECT_NE(read_ctx.callback, nullptr);
  EXPECT_EQ(read_ctx.fiber, nullptr);

  EXPECT_EQ(write_ctx.callback, nullptr);
  EXPECT_NE(write_ctx.fiber, nullptr);
}

// 测试23：部分事件删除
TEST_F(FdContextTest, PartialEventDeletion) {
  bool read_called = false;
  bool write_called = false;

  ctx1_->add_event(Channel::kRead);
  ctx1_->add_event(Channel::kWrite);

  auto &read_ctx = ctx1_->get_event_context(Channel::kRead);
  auto &write_ctx = ctx1_->get_event_context(Channel::kWrite);

  read_ctx.callback = [&read_called]() { read_called = true; };
  write_ctx.callback = [&write_called]() { write_called = true; };

  // 只删除读事件
  ctx1_->del_event(Channel::kRead);

  ctx1_->trigger_event(Channel::kRead);
  ctx1_->trigger_event(Channel::kWrite);

  EXPECT_FALSE(read_called); // 读事件已删除
  EXPECT_TRUE(write_called); // 写事件仍然有效
}

// ==================== 实际IO场景测试 ====================

// 测试24：模拟可读事件
TEST_F(FdContextTest, SimulateReadableEvent) {
  // 写入数据使fd1可读
  const char *msg = "test";
  ASSERT_EQ(write(fd2_, msg, strlen(msg)), strlen(msg));

  bool read_ready = false;
  ctx1_->add_event(Channel::kRead);
  auto &read_ctx = ctx1_->get_event_context(Channel::kRead);
  read_ctx.callback = [&read_ready]() { read_ready = true; };

  // 模拟epoll检测到可读并触发
  ctx1_->trigger_event(Channel::kRead);

  EXPECT_TRUE(read_ready);

  // 验证确实可读
  char buf[64];
  ssize_t n = read(fd1_, buf, sizeof(buf));
  EXPECT_GT(n, 0);
}

// 测试25：模拟可写事件
TEST_F(FdContextTest, SimulateWritableEvent) {
  // socket通常默认可写
  bool write_ready = false;

  ctx1_->add_event(Channel::kWrite);
  auto &write_ctx = ctx1_->get_event_context(Channel::kWrite);
  write_ctx.callback = [&write_ready]() { write_ready = true; };

  ctx1_->trigger_event(Channel::kWrite);

  EXPECT_TRUE(write_ready);

  // 验证确实可写
  const char *msg = "test";
  ssize_t n = write(fd1_, msg, strlen(msg));
  EXPECT_GT(n, 0);
}

// 测试26：多个FdContext独立性
TEST_F(FdContextTest, MultipleFdContextIndependence) {
  bool fd1_read = false;
  bool fd2_read = false;

  ctx1_->add_event(Channel::kRead);
  ctx2_->add_event(Channel::kRead);

  auto &ctx1_read = ctx1_->get_event_context(Channel::kRead);
  auto &ctx2_read = ctx2_->get_event_context(Channel::kRead);

  ctx1_read.callback = [&fd1_read]() { fd1_read = true; };
  ctx2_read.callback = [&fd2_read]() { fd2_read = true; };

  // 触发fd1的读事件
  ctx1_->trigger_event(Channel::kRead);

  EXPECT_TRUE(fd1_read);
  EXPECT_FALSE(fd2_read);

  // 触发fd2的读事件
  ctx2_->trigger_event(Channel::kRead);

  EXPECT_TRUE(fd1_read);
  EXPECT_TRUE(fd2_read);
}

// 测试27：事件回调中修改事件
TEST_F(FdContextTest, ModifyEventsInCallback) {
  int callback_count = 0;

  ctx1_->add_event(Channel::kRead);
  auto &read_ctx = ctx1_->get_event_context(Channel::kRead);

  read_ctx.callback = [this, &callback_count]() {
    callback_count++;
    // 在回调中删除读事件并添加写事件
    ctx1_->del_event(Channel::kRead);
    ctx1_->add_event(Channel::kWrite);
  };

  ctx1_->trigger_event(Channel::kRead);

  EXPECT_EQ(callback_count, 1);
  EXPECT_EQ(ctx1_->events(), Channel::kWrite);
}

// 测试28：协程和回调同时设置
TEST_F(FdContextTest, BothFiberAndCallbackSet) {
  bool callback_called = false;
  bool fiber_executed = false;

  ctx1_->add_event(Channel::kRead);
  auto &read_ctx = ctx1_->get_event_context(Channel::kRead);

  read_ctx.callback = [&callback_called]() { callback_called = true; };
  read_ctx.fiber =
      std::make_shared<Fiber>([&fiber_executed]() { fiber_executed = true; });

  ctx1_->trigger_event(Channel::kRead);

  // 至少应该执行其中一个
  EXPECT_TRUE(callback_called || fiber_executed);
}

int main(int argc, char **argv) {
  // 初始化日志系统
  zcoroutine::init_logger(zlog::LogLevel::value::DEBUG);

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}