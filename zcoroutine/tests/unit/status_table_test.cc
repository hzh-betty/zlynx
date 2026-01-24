/**
 * @file status_table_test.cc
 * @brief StatusTable 和 SocketStatus 单元测试
 * 测试文件描述符管理、超时设置、非阻塞状态管理等功能
 */

#include "hook.h"
#include "status_table.h"
#include "zcoroutine_logger.h"
#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace zcoroutine;

class StatusTableTest : public ::testing::Test {
protected:
  void SetUp() override {
    set_hook_enable(true);
    status_table_ = StatusTable::GetInstance();
  }

  void TearDown() override { set_hook_enable(false); }

  StatusTable *status_table_;
};

// ==================== StatusTable 基础功能测试 ====================

// 测试1：获取单例
TEST_F(StatusTableTest, GetInstance) {
  auto instance1 = StatusTable::GetInstance();
  auto instance2 = StatusTable::GetInstance();

  EXPECT_EQ(instance1, instance2);
  EXPECT_NE(instance1, nullptr);
}

// 测试2：获取不存在的 SocketStatus
TEST_F(StatusTableTest, GetNonexistentSocketFdContext) {
  auto ctx = status_table_->get(9999, false);
  EXPECT_EQ(ctx, nullptr);
}

// 测试3：自动创建 SocketStatus
TEST_F(StatusTableTest, AutoCreateSocketFdContext) {
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GT(sockfd, 0);

  auto ctx = status_table_->get(sockfd, true);
  EXPECT_NE(ctx, nullptr);
  EXPECT_EQ(ctx->is_init(), true);

  close(sockfd);
}

// 测试4：删除 SocketStatus
TEST_F(StatusTableTest, DeleteSocketFdContext) {
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GT(sockfd, 0);

  auto ctx = status_table_->get(sockfd, true);
  EXPECT_NE(ctx, nullptr);

  status_table_->del(sockfd);

  ctx = status_table_->get(sockfd, false);
  EXPECT_EQ(ctx, nullptr);

  close(sockfd);
}

// 测试5：多次获取同一个fd返回相同的 SocketStatus
TEST_F(StatusTableTest, SameFdReturnsSameCtx) {
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GT(sockfd, 0);

  auto ctx1 = status_table_->get(sockfd, true);
  auto ctx2 = status_table_->get(sockfd, false);

  EXPECT_EQ(ctx1, ctx2);

  close(sockfd);
}

// ==================== SocketStatus 基础功能测试 ====================

// 测试6：SocketStatus 初始化 - socket类型
TEST_F(StatusTableTest, SocketFdContextInitSocket) {
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GT(sockfd, 0);

  auto ctx = std::make_shared<SocketStatus>(sockfd);

  EXPECT_TRUE(ctx->is_init());
  EXPECT_TRUE(ctx->is_socket());
  EXPECT_TRUE(ctx->get_sys_nonblock());
  EXPECT_FALSE(ctx->get_user_nonblock());
  EXPECT_FALSE(ctx->is_closed());

  close(sockfd);
}

// 测试7：SocketStatus 初始化 - 非socket类型
TEST_F(StatusTableTest, SocketFdContextInitNonSocket) {
  int pipe_fds[2];
  ASSERT_EQ(pipe(pipe_fds), 0);

  auto ctx = std::make_shared<SocketStatus>(pipe_fds[0]);

  EXPECT_TRUE(ctx->is_init());
  EXPECT_FALSE(ctx->is_socket());
  EXPECT_FALSE(ctx->get_sys_nonblock());

  close(pipe_fds[0]);
  close(pipe_fds[1]);
}

// 测试8：无效fd的 SocketStatus
TEST_F(StatusTableTest, InvalidSocketFdContext) {
  auto ctx = std::make_shared<SocketStatus>(-1);

  EXPECT_FALSE(ctx->is_init());
  EXPECT_FALSE(ctx->is_socket());
}

// ==================== 非阻塞状态管理测试 ====================

// 测试9：sys_nonblock初始状态
TEST_F(StatusTableTest, SysNonblockInitial) {
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GT(sockfd, 0);

  auto ctx = status_table_->get(sockfd, true);

  // socket应该被自动设置为sys_nonblock
  EXPECT_TRUE(ctx->get_sys_nonblock());

  // 验证实际的fd flags
  int flags = fcntl_f(sockfd, F_GETFL, 0);
  EXPECT_TRUE(flags & O_NONBLOCK);

  close(sockfd);
}

// 测试10：user_nonblock初始状态
TEST_F(StatusTableTest, UserNonblockInitial) {
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GT(sockfd, 0);

  auto ctx = status_table_->get(sockfd, true);

  // 用户视角应该是阻塞的
  EXPECT_FALSE(ctx->get_user_nonblock());

  close(sockfd);
}

// 测试11：设置user_nonblock
TEST_F(StatusTableTest, SetUserNonblock) {
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GT(sockfd, 0);

  auto ctx = status_table_->get(sockfd, true);

  ctx->set_user_nonblock(true);
  EXPECT_TRUE(ctx->get_user_nonblock());

  ctx->set_user_nonblock(false);
  EXPECT_FALSE(ctx->get_user_nonblock());

  close(sockfd);
}

// 测试12：user_nonblock不影响sys_nonblock
TEST_F(StatusTableTest, UserNonblockIndependentOfSys) {
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GT(sockfd, 0);

  auto ctx = status_table_->get(sockfd, true);

  EXPECT_TRUE(ctx->get_sys_nonblock());
  EXPECT_FALSE(ctx->get_user_nonblock());

  // 设置user_nonblock不应该改变sys_nonblock
  ctx->set_user_nonblock(true);
  EXPECT_TRUE(ctx->get_sys_nonblock());
  EXPECT_TRUE(ctx->get_user_nonblock());

  close(sockfd);
}

// ==================== 超时管理测试 ====================

// 测试13：默认超时值
TEST_F(StatusTableTest, DefaultTimeout) {
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GT(sockfd, 0);

  auto ctx = status_table_->get(sockfd, true);

  EXPECT_EQ(ctx->get_timeout(SO_RCVTIMEO), 0);
  EXPECT_EQ(ctx->get_timeout(SO_SNDTIMEO), 0);

  close(sockfd);
}

// 测试14：设置接收超时
TEST_F(StatusTableTest, SetRcvTimeout) {
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GT(sockfd, 0);

  auto ctx = status_table_->get(sockfd, true);

  ctx->set_timeout(SO_RCVTIMEO, 1000);
  EXPECT_EQ(ctx->get_timeout(SO_RCVTIMEO), 1000);

  // SO_SNDTIMEO不应该被影响
  EXPECT_EQ(ctx->get_timeout(SO_SNDTIMEO), 0);

  close(sockfd);
}

// 测试15：设置发送超时
TEST_F(StatusTableTest, SetSndTimeout) {
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GT(sockfd, 0);

  auto ctx = status_table_->get(sockfd, true);

  ctx->set_timeout(SO_SNDTIMEO, 2000);
  EXPECT_EQ(ctx->get_timeout(SO_SNDTIMEO), 2000);

  // SO_RCVTIMEO不应该被影响
  EXPECT_EQ(ctx->get_timeout(SO_RCVTIMEO), 0);

  close(sockfd);
}

// 测试16：同时设置两种超时
TEST_F(StatusTableTest, SetBothTimeouts) {
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GT(sockfd, 0);

  auto ctx = status_table_->get(sockfd, true);

  ctx->set_timeout(SO_RCVTIMEO, 1500);
  ctx->set_timeout(SO_SNDTIMEO, 2500);

  EXPECT_EQ(ctx->get_timeout(SO_RCVTIMEO), 1500);
  EXPECT_EQ(ctx->get_timeout(SO_SNDTIMEO), 2500);

  close(sockfd);
}

// 测试17：修改超时值
TEST_F(StatusTableTest, ModifyTimeout) {
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GT(sockfd, 0);

  auto ctx = status_table_->get(sockfd, true);

  ctx->set_timeout(SO_RCVTIMEO, 1000);
  EXPECT_EQ(ctx->get_timeout(SO_RCVTIMEO), 1000);

  // 修改超时
  ctx->set_timeout(SO_RCVTIMEO, 3000);
  EXPECT_EQ(ctx->get_timeout(SO_RCVTIMEO), 3000);

  close(sockfd);
}

// ==================== 边界情况测试 ====================

// 测试18：大fd值
TEST_F(StatusTableTest, LargeFdValue) {
  // 创建多个fd，确保fd值比较大
  std::vector<int> fds;
  for (int i = 0; i < 100; ++i) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd > 0) {
      fds.push_back(fd);
    }
  }

  if (!fds.empty()) {
    int large_fd = fds.back();
    auto ctx = status_table_->get(large_fd, true);
    EXPECT_NE(ctx, nullptr);
    EXPECT_TRUE(ctx->is_socket());
  }

  for (int fd : fds) {
    close(fd);
  }
}

// 测试19：fd复用（close后重新创建）
TEST_F(StatusTableTest, FdReuse) {
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GT(sockfd, 0);

  auto ctx1 = status_table_->get(sockfd, true);
  ctx1->set_timeout(SO_RCVTIMEO, 1000);

  int saved_fd = sockfd;
  close(sockfd);
  status_table_->del(saved_fd);

  // 重新创建socket，可能复用同一个fd值
  int new_sockfd = socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GT(new_sockfd, 0);

  auto ctx2 = status_table_->get(new_sockfd, true);

  // 如果fd值相同，应该是新的ctx（超时已清零）
  if (new_sockfd == saved_fd) {
    EXPECT_EQ(ctx2->get_timeout(SO_RCVTIMEO), 0);
  }

  close(new_sockfd);
}

// 测试20：多线程安全性（简单测试）
TEST_F(StatusTableTest, MultiThreadSafety) {
  std::vector<std::thread> threads;
  std::vector<int> fds;

  for (int i = 0; i < 10; ++i) {
    threads.emplace_back([this, &fds]() {
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd > 0) {
        auto ctx = status_table_->get(fd, true);
        EXPECT_NE(ctx, nullptr);

        ctx->set_timeout(SO_RCVTIMEO, 1000);
        EXPECT_EQ(ctx->get_timeout(SO_RCVTIMEO), 1000);

        fds.push_back(fd);
      }
    });
  }

  for (auto &t : threads) {
    t.join();
  }

  for (int fd : fds) {
    close(fd);
  }
}

// ==================== 与Hook集成测试 ====================

// 测试21：通过hook的socket创建 SocketStatus
TEST_F(StatusTableTest, HookedSocketCreatesCtx) {
  int sockfd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GT(sockfd, 0);

  // 通过hook应该自动创建 SocketStatus
  auto ctx = status_table_->get(sockfd);
  EXPECT_NE(ctx, nullptr);
  EXPECT_TRUE(ctx->is_socket());
  EXPECT_TRUE(ctx->get_sys_nonblock());

  ::close(sockfd);
}

// 测试22：非hook的socket不自动创建
TEST_F(StatusTableTest, NonHookedSocketNoAutoCreate) {
  set_hook_enable(false);

  int sockfd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GT(sockfd, 0);

  auto ctx = status_table_->get(sockfd, false);
  // 没有hook时，不会自动创建
  EXPECT_EQ(ctx, nullptr);

  ::close(sockfd);
  set_hook_enable(true);
}

// 测试23：fcntl通过 SocketStatus 工作
TEST_F(StatusTableTest, FcntlWorkWithSocketFdContext) {
  int sockfd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GT(sockfd, 0);

  auto ctx = status_table_->get(sockfd);
  ASSERT_NE(ctx, nullptr);

  // 用户设置非阻塞
  int flags = ::fcntl(sockfd, F_GETFL, 0);
  ::fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

  // SocketStatus 应该反映这个变化
  EXPECT_TRUE(ctx->get_user_nonblock());

  ::close(sockfd);
}

// 测试24：setsockopt通过 SocketStatus 工作
TEST_F(StatusTableTest, SetsockoptWorkWithSocketFdContext) {
  int sockfd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GT(sockfd, 0);

  auto ctx = status_table_->get(sockfd);
  ASSERT_NE(ctx, nullptr);

  // 设置超时
  struct timeval tv;
  tv.tv_sec = 2;
  tv.tv_usec = 500000; // 2.5秒
  ::setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  // SocketStatus 应该记录这个超时
  EXPECT_EQ(ctx->get_timeout(SO_RCVTIMEO), 2500);

  ::close(sockfd);
}

// 测试25：ioctl FIONBIO通过 SocketStatus 工作
TEST_F(StatusTableTest, IoctlWorkWithSocketFdContext) {
  int sockfd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GT(sockfd, 0);

  auto ctx = status_table_->get(sockfd);
  ASSERT_NE(ctx, nullptr);
  EXPECT_FALSE(ctx->get_user_nonblock());

  // 使用ioctl设置非阻塞
  int on = 1;
  ::ioctl(sockfd, FIONBIO, &on);

  EXPECT_TRUE(ctx->get_user_nonblock());

  ::close(sockfd);
}

int main(int argc, char **argv) {
  // 初始化日志系统
  zcoroutine::init_logger(zlog::LogLevel::value::DEBUG);

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
