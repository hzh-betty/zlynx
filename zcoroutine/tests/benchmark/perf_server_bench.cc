/**
 * @file perf_server_bench.cc
 * @brief 用于perf热点分析的HTTP服务器
 */

#include "hook/hook.h"
#include "io/io_scheduler.h"
#include "runtime/fiber.h"
#include "util/zcoroutine_logger.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace zcoroutine;

static int g_listen_fd = -1;
static IoScheduler::ptr g_io_scheduler = nullptr;
static std::atomic<bool> g_running{true};
static std::atomic<uint64_t> g_requests{0};

static const char *HTTP_RESPONSE = "HTTP/1.1 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: 13\r\n"
                                   "Connection: close\r\n"
                                   "\r\n"
                                   "Hello, World!";
static const size_t HTTP_RESPONSE_LEN = strlen(HTTP_RESPONSE);

void handle_client(int client_fd) {
  set_hook_enable(true);
  char buffer[4096];

  int ret = recv(client_fd, buffer, sizeof(buffer), 0);
  if (ret > 0) {
    send(client_fd, HTTP_RESPONSE, HTTP_RESPONSE_LEN, 0);
    g_requests.fetch_add(1, std::memory_order_relaxed);
  }
  close(client_fd);
}

void accept_connection();

void register_accept() {
  if (g_io_scheduler && g_running.load()) {
    g_io_scheduler->add_event(g_listen_fd, FdContext::kRead, accept_connection);
  }
}

void accept_connection() {
  if (!g_running.load())
    return;

  set_hook_enable(true);
  struct sockaddr_in addr;
  socklen_t len = sizeof(addr);

  int client_fd = accept(g_listen_fd, (struct sockaddr *)&addr, &len);
  if (client_fd >= 0) {
    int flags = fcntl(client_fd, F_GETFL, 0);
    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

    auto fiber =
        std::make_shared<Fiber>([client_fd]() { handle_client(client_fd); });
    g_io_scheduler->schedule(std::move(fiber));
  }
  register_accept();
}

void signal_handler(int) { g_running.store(false); }

int main(int argc, char *argv[]) {
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);
  signal(SIGPIPE, SIG_IGN);

  zcoroutine::init_logger(zlog::LogLevel::value::ERROR);

  int port = 9000;
  int threads = 4;
  bool shared_stack = false;
  int duration = 30;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
      port = atoi(argv[++i]);
    } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
      threads = atoi(argv[++i]);
    } else if (strcmp(argv[i], "-s") == 0) {
      shared_stack = true;
    } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
      duration = atoi(argv[++i]);
    }
  }

  std::cout << "Starting server: port=" << port << ", threads=" << threads
            << ", shared_stack=" << (shared_stack ? "true" : "false")
            << ", duration=" << duration << "s" << std::endl;

  // 创建socket
  g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  int yes = 1;
  setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = INADDR_ANY;

  if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    std::cerr << "bind failed: " << strerror(errno) << std::endl;
    return 1;
  }
  listen(g_listen_fd, 4096);
  fcntl(g_listen_fd, F_SETFL, fcntl(g_listen_fd, F_GETFL) | O_NONBLOCK);

  // 创建调度器
  g_io_scheduler =
      std::make_shared<IoScheduler>(threads, "PerfServer", shared_stack);
  g_io_scheduler->start();
  set_hook_enable(true);

  g_io_scheduler->add_event(g_listen_fd, FdContext::kRead, accept_connection);

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
  g_io_scheduler->stop();
  close(g_listen_fd);

  std::cout << "Total requests: " << g_requests.load() << std::endl;

  return 0;
}
