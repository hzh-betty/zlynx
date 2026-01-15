/**
 * @file perf_server_bench.cc
 * @brief 用于 perf 热点分析的 HTTP 服务器（基于 znet）
 */

#include "tcp_server.h"
#include "tcp_connection.h"
#include "address.h"
#include "buff.h"
#include "io/io_scheduler.h"
#include "znet_logger.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <signal.h>
#include <thread>
#include <unistd.h>

using namespace znet;
using namespace zcoroutine;

static TcpServer::ptr g_server = nullptr;
static std::atomic<bool> g_running{true};
static std::atomic<uint64_t> g_requests{0};

static const char *HTTP_RESPONSE = "HTTP/1.1 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: 13\r\n"
                                   "Connection: close\r\n"
                                   "\r\n"
                                   "Hello, World!";
static const size_t HTTP_RESPONSE_LEN = strlen(HTTP_RESPONSE);

// HTTP 服务器
class PerfHttpServer : public TcpServer {
public:
  using ptr = std::shared_ptr<PerfHttpServer>;

  PerfHttpServer(IoScheduler::ptr io_worker,
                 IoScheduler::ptr accept_worker = nullptr)
      : TcpServer(io_worker, accept_worker) {}

protected:
  void handle_client(TcpConnectionPtr conn) override {
    if (!g_running.load()) {
      return;
    }

    // 设置消息回调
    conn->set_message_callback(
        [](const TcpConnectionPtr &conn, Buffer *buf) {
          size_t readable = buf->readable_bytes();
          if (readable > 0) {
            // 简单检查是否收到完整HTTP请求
            const char *crlf = buf->find_crlf();
            if (crlf) {
              buf->retrieve_all();
              
              // 发送响应
              conn->send(HTTP_RESPONSE, HTTP_RESPONSE_LEN);
              g_requests.fetch_add(1, std::memory_order_relaxed);
              
              // 短连接
              conn->shutdown();
            }
          }
        });

    conn->set_connection_callback([](const TcpConnectionPtr &conn) {
      if (conn->connected()) {
        // 连接建立
      }
    });
  }
};

void signal_handler(int) { g_running.store(false); }

int main(int argc, char *argv[]) {
  signal(SIGPIPE, SIG_IGN);

  // 初始化日志系统（同时初始化 znet 和 zcoroutine）
  znet::init_logger(zlog::LogLevel::value::WARNING);

  int port = 9000;
  int threads = 4;
  int duration = 30;
  bool use_shared_stack = false;

  // 解析命令行参数
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
      port = atoi(argv[++i]);
    } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
      threads = atoi(argv[++i]);
    } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
      duration = atoi(argv[++i]);
    } else if (strcmp(argv[i], "-s") == 0) {
      use_shared_stack = true;
    } else if (strcmp(argv[i], "-h") == 0) {
      std::cout << "Usage: " << argv[0] << " [options]\n"
                << "Options:\n"
                << "  -p <port>      端口 (default: 9000)\n"
                << "  -t <threads>   线程数 (default: 4)\n"
                << "  -d <duration>  运行时长(秒) (default: 30)\n"
                << "  -s             使用共享栈模式 (default: 独立栈)\n"
                << "  -h             显示帮助\n";
      return 0;
    }
  }

  std::cout << "Starting server: port=" << port << ", threads=" << threads
            << ", shared_stack=" << (use_shared_stack ? "true" : "false")
            << ", duration=" << duration << "s" << std::endl;

  // 创建IO调度器
  auto io_worker =
      std::make_shared<IoScheduler>(threads, "PerfWorker", use_shared_stack);

  auto accept_worker =
      std::make_shared<IoScheduler>(1, "PerfAcceptor", use_shared_stack);

  // 创建服务器
  g_server = std::make_shared<PerfHttpServer>(io_worker, accept_worker);
  g_server->set_name("PerfHttpServer");

  auto addr = std::make_shared<IPv4Address>("0.0.0.0", static_cast<uint16_t>(port));
  if (!addr) {
    std::cerr << "Invalid address" << std::endl;
    return 1;
  }

  if (!g_server->bind(addr)) {
    std::cerr << "bind failed" << std::endl;
    return 1;
  }

  if (!g_server->start()) {
    std::cerr << "start failed" << std::endl;
    return 1;
  }

  std::cout << "Server running, press Ctrl+C to stop or wait " << duration
            << "s..." << std::endl;

  // 运行指定时间
  auto start = std::chrono::steady_clock::now();
  while (g_running.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - start).count() >=
        duration) {
      break;
    }
  }

  g_running.store(false);
  g_server->stop();
  g_server.reset();

  std::cout << "Total requests: " << g_requests.load() << std::endl;

  return 0;
}
