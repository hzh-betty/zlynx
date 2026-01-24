/**
 * @file http_bench.cc
 * @brief 基于 znet 的 HTTP 服务器性能基准测试
 *
 * 测试场景：
 * 1. HTTP 短连接性能测试
 * 2. 高并发连接处理
 * 3. 与 wrk 工具集成进行压测
 */

#include "address.h"
#include "buff.h"
#include "io_scheduler.h"
#include "tcp_connection.h"
#include "tcp_server.h"
#include "znet_logger.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <signal.h>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

using namespace znet;
using namespace zcoroutine;

// ========================= 全局配置 =========================
struct BenchConfig {
  int port = 9000;           // 服务器端口
  int thread_num = 4;        // 工作线程数
  int wrk_threads = 4;       // wrk压测线程数
  int wrk_connections = 100; // wrk并发连接数
  int wrk_duration = 10;     // wrk压测时长（秒）
  std::string test_name;     // 测试名称
};

// ========================= 统计数据 =========================
struct BenchStats {
  std::atomic<uint64_t> requests_handled{0};
  std::atomic<uint64_t> bytes_sent{0};
  std::atomic<uint64_t> bytes_received{0};
  std::atomic<uint64_t> connections_accepted{0};
  std::chrono::steady_clock::time_point start_time;
  std::chrono::steady_clock::time_point end_time;

  void reset() {
    requests_handled.store(0);
    bytes_sent.store(0);
    bytes_received.store(0);
    connections_accepted.store(0);
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
static TcpServer::ptr g_server = nullptr;
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

// ========================= HTTP 处理 =========================

// 简单的 HTTP 请求处理器
class HttpServer : public TcpServer {
public:
  using ptr = std::shared_ptr<HttpServer>;

  HttpServer(IoScheduler::ptr io_worker,
             IoScheduler::ptr accept_worker = nullptr)
      : TcpServer(io_worker, accept_worker) {}

protected:
  void handle_client(TcpConnectionPtr conn) override {
    if (!g_running.load(std::memory_order_relaxed)) {
      return;
    }

    g_stats.connections_accepted.fetch_add(1, std::memory_order_relaxed);

    // 设置回调
    conn->set_message_callback([](const TcpConnectionPtr &conn, Buffer *buf) {
      size_t readable = buf->readable_bytes();
      if (readable > 0) {
        g_stats.bytes_received.fetch_add(readable, std::memory_order_relaxed);

        // 简单检查是否收到完整HTTP请求（查找\r\n\r\n）
        const char *crlf = buf->find_crlf();
        if (crlf) {
          // 清空输入缓冲区
          buf->retrieve_all();

          // 发送HTTP响应
          conn->send(HTTP_RESPONSE, HTTP_RESPONSE_LEN);
          g_stats.bytes_sent.fetch_add(HTTP_RESPONSE_LEN,
                                       std::memory_order_relaxed);
          g_stats.requests_handled.fetch_add(1, std::memory_order_relaxed);

          // 短连接：关闭连接
          conn->shutdown();
        }
      }
    });

    conn->set_connection_callback([](const TcpConnectionPtr &conn) {
      if (conn->connected()) {
        // 连接建立
      }
    });

    conn->set_close_callback([](const TcpConnectionPtr &conn) {
      (void)conn; // 连接关闭回调，参数保留用于未来扩展
    });
  }
};

// ========================= 服务器管理 =========================

void start_server(const BenchConfig &config) {

  auto io_worker =
      std::make_shared<IoScheduler>(config.thread_num, "HttpWorker", true);
  auto accept_worker = std::make_shared<IoScheduler>(1, "HttpAcceptor", true);

  g_server = std::make_shared<HttpServer>(io_worker, accept_worker);
  g_server->set_name("HttpBenchServer");

  auto addr = std::make_shared<IPv4Address>("0.0.0.0",
                                            static_cast<uint16_t>(config.port));
  if (!addr) {
    std::cerr << "Invalid address" << std::endl;
    exit(1);
  }

  if (!g_server->bind(addr)) {
    std::cerr << "Failed to bind to port " << config.port << std::endl;
    exit(1);
  }

  if (!g_server->start()) {
    std::cerr << "Failed to start server" << std::endl;
    exit(1);
  }

  g_running.store(true, std::memory_order_release);
  g_stats.reset();

  std::cout << "Server started on port " << config.port << std::endl;
}

void stop_server() {
  g_running.store(false, std::memory_order_release);
  g_stats.finish();

  if (g_server) {
    g_server->stop();
    g_server.reset();
  }
}

// ========================= WRK 压测 =========================

struct WrkResult {
  double requests_per_sec = 0;
  uint64_t total_requests = 0;
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

// ========================= 主测试函数 =========================

void run_http_benchmark(BenchConfig config) {
  std::cout << "\n========================================" << std::endl;
  std::cout << "HTTP基准测试: " << config.test_name << std::endl;
  std::cout << "  端口: " << config.port << std::endl;
  std::cout << "  工作线程: " << config.thread_num << std::endl;
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
  std::cout << "znet HTTP 性能基准测试" << std::endl;
  std::cout << std::string(60, '=') << std::endl;
  std::cout << "测试时间: "
            << std::chrono::system_clock::now().time_since_epoch().count()
            << std::endl;
  std::cout << "线程数: " << thread_num << std::endl;
  std::cout << "wrk测试时长: " << wrk_duration << "s" << std::endl;
  std::cout << std::string(60, '=') << "\n" << std::endl;

  // 测试1: HTTP短连接测试
  {
    BenchConfig config;
    config.port = 9001;
    config.thread_num = thread_num;
    config.wrk_duration = wrk_duration;
    config.wrk_threads = 4;
    config.wrk_connections = 100;
    config.test_name = "HTTP短连接测试";
    run_http_benchmark(config);
  }

  std::this_thread::sleep_for(std::chrono::seconds(2));

  // 测试2: 高并发连接测试
  {
    BenchConfig config;
    config.port = 9002;
    config.thread_num = thread_num;
    config.wrk_duration = wrk_duration;
    config.wrk_threads = 8;
    config.wrk_connections = 500;
    config.test_name = "高并发连接测试";
    run_http_benchmark(config);
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
  znet::init_logger(zlog::LogLevel::value::WARNING);

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
