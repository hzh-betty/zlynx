/**
 * @file http_bench.cc
 * @brief HTTP服务器性能基准测试
 *
 * 测试场景：
 * 1. 独立栈模式（Independent Stack）HTTP短连接
 * 2. 共享栈模式（Shared Stack）HTTP短连接
 * 3. 定时器密集场景
 * 4. IO事件密集场景
 *
 * 使用fork创建子进程运行wrk进行压测
 */

#include "hook.h"
#include "io_scheduler.h"
#include "fiber.h"
#include "scheduler.h"
#include "shared_stack.h"
#include "zcoroutine_logger.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <netinet/in.h>
#include <signal.h>
#include <sstream>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace zcoroutine;

// ========================= 全局配置 =========================
struct BenchConfig {
  int port = 9000;               // 服务器端口
  int thread_num = 4;            // 工作线程数
  int wrk_threads = 4;           // wrk压测线程数
  int wrk_connections = 100;     // wrk并发连接数
  int wrk_duration = 10;         // wrk压测时长（秒）
  bool use_shared_stack = false; // 是否使用共享栈
  std::string test_name;         // 测试名称
};

// ========================= 统计数据 =========================
struct BenchStats {
  std::atomic<uint64_t> requests_handled{0};
  std::atomic<uint64_t> bytes_sent{0};
  std::atomic<uint64_t> bytes_received{0};
  std::atomic<uint64_t> connections_accepted{0};
  std::atomic<uint64_t> fiber_created{0};
  std::atomic<uint64_t> timer_fired{0};
  std::chrono::steady_clock::time_point start_time;
  std::chrono::steady_clock::time_point end_time;

  void reset() {
    requests_handled.store(0);
    bytes_sent.store(0);
    bytes_received.store(0);
    connections_accepted.store(0);
    fiber_created.store(0);
    timer_fired.store(0);
    start_time = std::chrono::steady_clock::now();
  }

  void finish() { end_time = std::chrono::steady_clock::now(); }

  double elapsed_seconds() const {
    return std::chrono::duration<double>(end_time - start_time).count();
  }

  void print(const std::string &test_name) const {
    double elapsed = elapsed_seconds();
    std::cout << "\n========== " << test_name
              << " 结果 ==========" << std::endl;
    std::cout << "测试时长: " << std::fixed << std::setprecision(2) << elapsed
              << " 秒" << std::endl;
    std::cout << "处理请求数: " << requests_handled.load() << std::endl;
    std::cout << "接收连接数: " << connections_accepted.load() << std::endl;
    std::cout << "创建Fiber数: " << fiber_created.load() << std::endl;
    std::cout << "定时器触发数: " << timer_fired.load() << std::endl;
    std::cout << "发送字节数: " << bytes_sent.load() << std::endl;
    std::cout << "接收字节数: " << bytes_received.load() << std::endl;
    if (elapsed > 0) {
      std::cout << "RPS: " << std::fixed << std::setprecision(2)
                << (requests_handled.load() / elapsed) << std::endl;
      std::cout << "吞吐量: " << std::fixed << std::setprecision(2)
                << ((bytes_sent.load() + bytes_received.load()) / elapsed /
                    1024 / 1024)
                << " MB/s" << std::endl;
    }
    std::cout << "==========================================\n" << std::endl;
  }
};

// 全局变量
static int g_listen_fd = -1;
static IoScheduler::ptr g_io_scheduler = nullptr;
static BenchStats g_stats;
static std::atomic<bool> g_running{false};

// HTTP响应内容
static const char *HTTP_RESPONSE = "HTTP/1.1 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: 13\r\n"
                                   "Connection: close\r\n"
                                   "\r\n"
                                   "Hello, World!";

static const size_t HTTP_RESPONSE_LEN = strlen(HTTP_RESPONSE);

// ========================= HTTP处理 =========================

// 处理客户端连接（在协程中运行）
void handle_client_fiber(int client_fd) {
  g_stats.fiber_created.fetch_add(1, std::memory_order_relaxed);
  set_hook_enable(true);

  char buffer[4096] = {0};

  // 读取HTTP请求
  int ret = recv(client_fd, buffer, sizeof(buffer), 0);

  if (ret > 0) {
    g_stats.bytes_received.fetch_add(ret, std::memory_order_relaxed);

    // 发送HTTP响应
    int send_ret = send(client_fd, HTTP_RESPONSE, HTTP_RESPONSE_LEN, 0);
    if (send_ret > 0) {
      g_stats.bytes_sent.fetch_add(send_ret, std::memory_order_relaxed);
      g_stats.requests_handled.fetch_add(1, std::memory_order_relaxed);
    }
  }

  close(client_fd);
}

// 前向声明
void accept_connection();

// 注册监听socket的读事件
void register_accept_event() {
  if (g_io_scheduler && g_running.load(std::memory_order_relaxed)) {
    g_io_scheduler->add_event(g_listen_fd, FdContext::kRead, accept_connection);
  }
}

// accept回调函数
void accept_connection() {
  if (!g_running.load(std::memory_order_relaxed)) {
    return;
  }

  set_hook_enable(true);
  struct sockaddr_in client_addr;
  socklen_t client_len = sizeof(client_addr);
  memset(&client_addr, 0, sizeof(client_addr));

  // 接受连接
  int client_fd =
      accept(g_listen_fd, (struct sockaddr *)&client_addr, &client_len);

  if (client_fd < 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      // 忽略错误
    }
    register_accept_event();
    return;
  }

  g_stats.connections_accepted.fetch_add(1, std::memory_order_relaxed);

  // 设置客户端socket为非阻塞
  int flags = fcntl(client_fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
  }

  // 创建Fiber处理客户端
  if (g_io_scheduler) {
    Fiber::ptr client_fiber = std::make_shared<Fiber>(
        [client_fd]() { handle_client_fiber(client_fd); });
    g_io_scheduler->schedule(std::move(client_fiber));
  }

  register_accept_event();
}

// ========================= 服务器管理 =========================

int create_server_socket(int port) {
  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    std::cerr << "Failed to create socket" << std::endl;
    return -1;
  }

  // 设置SO_REUSEADDR和SO_REUSEPORT
  int yes = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port);
  server_addr.sin_addr.s_addr = INADDR_ANY;

  if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) <
      0) {
    std::cerr << "Failed to bind, errno=" << errno << std::endl;
    close(listen_fd);
    return -1;
  }

  if (listen(listen_fd, 4096) < 0) {
    std::cerr << "Failed to listen" << std::endl;
    close(listen_fd);
    return -1;
  }

  // 设置为非阻塞
  int flags = fcntl(listen_fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);
  }

  return listen_fd;
}

void start_server(const BenchConfig &config) {
  g_listen_fd = create_server_socket(config.port);
  if (g_listen_fd < 0) {
    std::cerr << "Failed to create server socket" << std::endl;
    exit(1);
  }

  // 创建IoScheduler
  g_io_scheduler = std::make_shared<IoScheduler>(
      config.thread_num, "BenchServer", config.use_shared_stack);
  g_io_scheduler->start();

  set_hook_enable(true);
  g_running.store(true, std::memory_order_release);
  g_stats.reset();

  // 注册accept事件
  g_io_scheduler->add_event(g_listen_fd, FdContext::kRead, accept_connection);

  std::cout << "Server started on port " << config.port
            << " (shared_stack=" << (config.use_shared_stack ? "true" : "false")
            << ")" << std::endl;
}

void stop_server() {
  g_running.store(false, std::memory_order_release);
  g_stats.finish();

  if (g_io_scheduler) {
    g_io_scheduler->stop();
    g_io_scheduler.reset();
  }

  if (g_listen_fd >= 0) {
    close(g_listen_fd);
    g_listen_fd = -1;
  }
}

// ========================= WRK压测 =========================

struct WrkResult {
  double requests_per_sec = 0;
  double avg_latency_ms = 0;
  double max_latency_ms = 0;
  uint64_t total_requests = 0;
  double transfer_rate_mb = 0;
  bool success = false;
};

WrkResult run_wrk(const BenchConfig &config) {
  WrkResult result;

  std::ostringstream cmd;
  cmd << "wrk -t" << config.wrk_threads << " -c" << config.wrk_connections
      << " -d" << config.wrk_duration << "s"
      << " http://127.0.0.1:" << config.port << "/" << " 2>&1";

  std::cout << "Running: " << cmd.str() << std::endl;

  pid_t pid = fork();
  if (pid == 0) {
    // 子进程
    // 等待服务器完全启动
    usleep(500000); // 500ms

    execlp("sh", "sh", "-c", cmd.str().c_str(), nullptr);
    _exit(1);
  } else if (pid > 0) {
    // 父进程等待
    int status;
    waitpid(pid, &status, 0);
    result.success = WIFEXITED(status) && WEXITSTATUS(status) == 0;
  } else {
    std::cerr << "fork failed" << std::endl;
    result.success = false;
  }

  return result;
}

// ========================= 定时器密集测试 =========================

void timer_intensive_test(const BenchConfig &config) {
  std::cout << "\n>>> 定时器密集测试 (" << config.test_name << ") <<<"
            << std::endl;

  auto scheduler = std::make_shared<IoScheduler>(
      config.thread_num, "TimerBench", config.use_shared_stack);
  scheduler->start();

  std::atomic<uint64_t> timer_count{0};
  const int total_timers = 10000;
  const int duration_ms = 5000;

  auto start = std::chrono::steady_clock::now();

  // 添加大量定时器
  for (int i = 0; i < total_timers; ++i) {
    int timeout = (i % 100) + 1; // 1-100ms随机超时
    scheduler->add_timer(timeout, [&timer_count]() {
      timer_count.fetch_add(1, std::memory_order_relaxed);
    });
  }

  // 等待定时器执行
  std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));

  scheduler->stop();

  auto end = std::chrono::steady_clock::now();
  double elapsed =
      std::chrono::duration<double, std::milli>(end - start).count();

  std::cout << "定时器密集测试结果:" << std::endl;
  std::cout << "  总定时器数: " << total_timers << std::endl;
  std::cout << "  触发定时器数: " << timer_count.load() << std::endl;
  std::cout << "  测试耗时: " << elapsed << " ms" << std::endl;
  std::cout << "  定时器/秒: " << (timer_count.load() * 1000.0 / elapsed)
            << std::endl;
}

// ========================= Fiber密集测试 =========================

void fiber_intensive_test(const BenchConfig &config) {
  std::cout << "\n>>> Fiber密集测试 (" << config.test_name << ") <<<"
            << std::endl;

  auto scheduler = std::make_shared<Scheduler>(config.thread_num, "FiberBench",
                                               config.use_shared_stack);
  scheduler->start();

  std::atomic<uint64_t> fiber_count{0};
  const int total_fibers = 50000;

  auto start = std::chrono::steady_clock::now();

  // 创建大量Fiber
  for (int i = 0; i < total_fibers; ++i) {
    scheduler->schedule([&fiber_count]() {
      fiber_count.fetch_add(1, std::memory_order_relaxed);
      // 模拟一些工作
      for (volatile int j = 0; j < 100; ++j) {
      }
      Fiber::yield();
    });
  }

  // 等待Fiber执行完成
  while (scheduler->pending_task_count() > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  scheduler->stop();

  auto end = std::chrono::steady_clock::now();
  double elapsed =
      std::chrono::duration<double, std::milli>(end - start).count();

  std::cout << "Fiber密集测试结果:" << std::endl;
  std::cout << "  总Fiber数: " << total_fibers << std::endl;
  std::cout << "  完成Fiber数: " << fiber_count.load() << std::endl;
  std::cout << "  测试耗时: " << elapsed << " ms" << std::endl;
  std::cout << "  Fiber/秒: " << (fiber_count.load() * 1000.0 / elapsed)
            << std::endl;
}

// ========================= 上下文切换测试 =========================

void context_switch_test(const BenchConfig &config) {
  std::cout << "\n>>> 上下文切换测试 (" << config.test_name << ") <<<"
            << std::endl;

  auto scheduler =
      std::make_shared<Scheduler>(1, "SwitchBench", config.use_shared_stack);
  scheduler->start();

  std::atomic<uint64_t> switch_count{0};
  int switches_per_fiber = 1000;
  const int num_fibers = 10;

  auto start = std::chrono::steady_clock::now();

  for (int f = 0; f < num_fibers; ++f) {
    scheduler->schedule([&switch_count, switches_per_fiber]() {
      for (int i = 0; i < switches_per_fiber; ++i) {
        switch_count.fetch_add(1, std::memory_order_relaxed);
        Fiber::yield();
      }
    });
  }

  // 等待所有Fiber完成
  while (scheduler->pending_task_count() > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  scheduler->stop();

  auto end = std::chrono::steady_clock::now();
  double elapsed =
      std::chrono::duration<double, std::milli>(end - start).count();

  std::cout << "上下文切换测试结果:" << std::endl;
  std::cout << "  总切换次数: " << switch_count.load() << std::endl;
  std::cout << "  测试耗时: " << elapsed << " ms" << std::endl;
  std::cout << "  切换/秒: " << (switch_count.load() * 1000.0 / elapsed)
            << std::endl;
  std::cout << "  每次切换耗时: " << (elapsed * 1000.0 / switch_count.load())
            << " us" << std::endl;
}

// ========================= 主测试函数 =========================

void run_http_benchmark(BenchConfig config) {
  std::cout << "\n========================================" << std::endl;
  std::cout << "HTTP基准测试: " << config.test_name << std::endl;
  std::cout << "  端口: " << config.port << std::endl;
  std::cout << "  工作线程: " << config.thread_num << std::endl;
  std::cout << "  共享栈: " << (config.use_shared_stack ? "是" : "否")
            << std::endl;
  std::cout << "  wrk线程: " << config.wrk_threads << std::endl;
  std::cout << "  wrk连接: " << config.wrk_connections << std::endl;
  std::cout << "  wrk时长: " << config.wrk_duration << "s" << std::endl;
  std::cout << "========================================\n" << std::endl;

  // 启动服务器
  start_server(config);

  // 运行wrk压测
  run_wrk(config);

  // 停止服务器
  stop_server();

  // 打印统计
  g_stats.print(config.test_name);
}

void run_all_benchmarks(int wrk_duration, int thread_num) {
  std::cout << "\n" << std::string(60, '=') << std::endl;
  std::cout << "zcoroutine 性能基准测试" << std::endl;
  std::cout << std::string(60, '=') << std::endl;
  std::cout << "测试时间: "
            << std::chrono::system_clock::now().time_since_epoch().count()
            << std::endl;
  std::cout << "线程数: " << thread_num << std::endl;
  std::cout << "wrk测试时长: " << wrk_duration << "s" << std::endl;
  std::cout << std::string(60, '=') << "\n" << std::endl;

  // 测试1: 独立栈HTTP测试
  {
    BenchConfig config;
    config.port = 9001;
    config.thread_num = thread_num;
    config.use_shared_stack = false;
    config.wrk_duration = wrk_duration;
    config.wrk_threads = 4;
    config.wrk_connections = 100;
    config.test_name = "独立栈HTTP测试";
    run_http_benchmark(config);
  }

  std::this_thread::sleep_for(std::chrono::seconds(2));

  // 测试2: 共享栈HTTP测试
  {
    BenchConfig config;
    config.port = 9002;
    config.thread_num = thread_num;
    config.use_shared_stack = true;
    config.wrk_duration = wrk_duration;
    config.wrk_threads = 4;
    config.wrk_connections = 100;
    config.test_name = "共享栈HTTP测试";
    run_http_benchmark(config);
  }

  std::this_thread::sleep_for(std::chrono::seconds(1));

  // 测试3: 独立栈Fiber密集测试
  {
    BenchConfig config;
    config.thread_num = thread_num;
    config.use_shared_stack = false;
    config.test_name = "独立栈";
    fiber_intensive_test(config);
  }

  // 测试4: 共享栈Fiber密集测试
  {
    BenchConfig config;
    config.thread_num = thread_num;
    config.use_shared_stack = true;
    config.test_name = "共享栈";
    fiber_intensive_test(config);
  }

  // 测试5: 独立栈上下文切换测试
  {
    BenchConfig config;
    config.use_shared_stack = false;
    config.test_name = "独立栈";
    context_switch_test(config);
  }

  // 测试6: 共享栈上下文切换测试
  {
    BenchConfig config;
    config.use_shared_stack = true;
    config.test_name = "共享栈";
    context_switch_test(config);
  }

  // 测试7: 独立栈定时器测试
  {
    BenchConfig config;
    config.thread_num = thread_num;
    config.use_shared_stack = false;
    config.test_name = "独立栈";
    timer_intensive_test(config);
  }

  // 测试8: 共享栈定时器测试
  {
    BenchConfig config;
    config.thread_num = thread_num;
    config.use_shared_stack = true;
    config.test_name = "共享栈";
    timer_intensive_test(config);
  }

  std::cout << "\n" << std::string(60, '=') << std::endl;
  std::cout << "所有测试完成" << std::endl;
  std::cout << std::string(60, '=') << "\n" << std::endl;
}

// ========================= 主函数 =========================

void print_usage(const char *prog) {
  std::cout << "Usage: " << prog << " [options]\n"
            << "Options:\n"
            << "  -t <threads>    工作线程数 (default: 4)\n"
            << "  -d <duration>   wrk测试时长秒 (default: 10)\n"
            << "  -h              显示帮助\n";
}

int main(int argc, char *argv[]) {
  // 忽略SIGPIPE信号
  signal(SIGPIPE, SIG_IGN);

  // 初始化日志系统（WARNING级别以避免太多日志输出）
  zcoroutine::init_logger(zlog::LogLevel::value::WARNING);

  int thread_num = 4;
  int wrk_duration = 10;

  // 解析命令行参数
  int opt;
  while ((opt = getopt(argc, argv, "t:d:h")) != -1) {
    switch (opt) {
    case 't':
      thread_num = std::atoi(optarg);
      break;
    case 'd':
      wrk_duration = std::atoi(optarg);
      break;
    case 'h':
      print_usage(argv[0]);
      return 0;
    default:
      print_usage(argv[0]);
      return 1;
    }
  }

  // 检查wrk是否可用
  if (system("which wrk > /dev/null 2>&1") != 0) {
    std::cerr << "错误: 未找到wrk命令，请先安装wrk" << std::endl;
    std::cerr << "Ubuntu/Debian: sudo apt-get install wrk" << std::endl;
    return 1;
  }

  // 运行所有基准测试
  run_all_benchmarks(wrk_duration, thread_num);

  return 0;
}
