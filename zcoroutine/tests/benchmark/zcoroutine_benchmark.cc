/**
 * zcoroutine_benchmark.cc
 *
 * 目标：对 zcoroutine 的 HTTP 短连接场景进行性能/压力测试。
 * - 通过 IoScheduler 运行一个最小 HTTP 服务器
 * - fork 子进程 exec wrk 进行压测（可通过命令行控制 wrk 参数）
 * - 支持独立栈与共享栈两种模式
 */

#include "fiber.h"
#include "fiber_pool.h"
#include "hook.h"
#include "io_scheduler.h"
#include "zcoroutine_logger.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace zcoroutine;

namespace {

struct BenchConfig {
  int port = 9000;
  int thread_num = 4;
  bool use_shared_stack = false;

  bool run_wrk = true;
  std::string wrk_bin = "wrk";
  int wrk_threads = 4;
  int wrk_connections = 200;
  std::string wrk_duration = "15s";
  std::string path = "/";
  int warmup_ms = 500;
  std::vector<std::string> wrk_args; // 透传给 wrk 的额外参数（可重复）

  // 仅 server-only 模式使用
  int run_seconds = 60;
};

struct BenchStats {
  std::atomic<uint64_t> connections_accepted{0};
  std::atomic<uint64_t> requests_handled{0};
  std::atomic<uint64_t> bytes_received{0};
  std::atomic<uint64_t> bytes_sent{0};
  std::atomic<uint64_t> fibers_created{0};

  std::chrono::steady_clock::time_point start_time;
  std::chrono::steady_clock::time_point end_time;

  void reset() {
    connections_accepted.store(0, std::memory_order_relaxed);
    requests_handled.store(0, std::memory_order_relaxed);
    bytes_received.store(0, std::memory_order_relaxed);
    bytes_sent.store(0, std::memory_order_relaxed);
    fibers_created.store(0, std::memory_order_relaxed);
    start_time = std::chrono::steady_clock::now();
  }

  void finish() { end_time = std::chrono::steady_clock::now(); }

  double elapsed_seconds() const {
    return std::chrono::duration<double>(end_time - start_time).count();
  }
};

static int g_listen_fd = -1;
static IoScheduler::ptr g_io_scheduler;
static std::atomic<bool> g_running{false};
static BenchStats g_stats;

static const char *kHttpResponse = "HTTP/1.1 200 OK\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Content-Length: 2\r\n"
                                   "Connection: close\r\n"
                                   "\r\n"
                                   "OK";

void usage(const char *prog) {
  std::cout
      << "Usage: " << prog << " [options]\n\n"
      << "Server options:\n"
      << "  --port N                 Listen port (default 9000)\n"
      << "  --threads N              IoScheduler worker threads (default 4)\n"
      << "  --stack [independent|shared]  Stack mode (default independent)\n"
      << "\nWrk options (when not --no-wrk):\n"
      << "  --wrk-bin PATH            wrk binary name/path (default wrk)\n"
      << "  --wrk-threads N           wrk threads (default 4)\n"
      << "  --wrk-connections N       wrk connections (default 200)\n"
      << "  --wrk-duration STR        wrk duration (default 15s, e.g. 10s/1m)\n"
      << "  --path STR                Request path (default /)\n"
      << "  --warmup-ms N             Delay before exec wrk (default 500)\n"
      << "  --wrk-arg ARG             Extra arg forwarded to wrk (repeatable)\n"
      << "\nModes:\n"
      << "  --no-wrk                  Only run server\n"
      << "  --run-seconds N           Server-only runtime (default 60)\n"
      << "\nOther:\n"
      << "  -h, --help                Show help\n";
}

bool starts_with(const char *s, const char *prefix) {
  return s && prefix && std::strncmp(s, prefix, std::strlen(prefix)) == 0;
}

bool parse_int(const char *s, int &out) {
  if (!s || *s == '\0')
    return false;
  char *end = nullptr;
  long v = std::strtol(s, &end, 10);
  if (!end || *end != '\0')
    return false;
  out = static_cast<int>(v);
  return true;
}

BenchConfig parse_args(int argc, char **argv) {
  BenchConfig cfg;

  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    if (!arg)
      continue;

    if (std::strcmp(arg, "-h") == 0 || std::strcmp(arg, "--help") == 0) {
      usage(argv[0]);
      std::exit(0);
    }

    if (std::strcmp(arg, "--port") == 0 && i + 1 < argc) {
      parse_int(argv[++i], cfg.port);
      continue;
    }
    if (std::strcmp(arg, "--threads") == 0 && i + 1 < argc) {
      parse_int(argv[++i], cfg.thread_num);
      continue;
    }
    if (std::strcmp(arg, "--stack") == 0 && i + 1 < argc) {
      std::string mode = argv[++i];
      if (mode == "shared") {
        cfg.use_shared_stack = true;
      } else if (mode == "independent") {
        cfg.use_shared_stack = false;
      } else {
        std::cerr << "Unknown --stack value: " << mode << std::endl;
        std::exit(2);
      }
      continue;
    }

    if (std::strcmp(arg, "--wrk-bin") == 0 && i + 1 < argc) {
      cfg.wrk_bin = argv[++i];
      continue;
    }
    if (std::strcmp(arg, "--wrk-threads") == 0 && i + 1 < argc) {
      parse_int(argv[++i], cfg.wrk_threads);
      continue;
    }
    if (std::strcmp(arg, "--wrk-connections") == 0 && i + 1 < argc) {
      parse_int(argv[++i], cfg.wrk_connections);
      continue;
    }
    if (std::strcmp(arg, "--wrk-duration") == 0 && i + 1 < argc) {
      cfg.wrk_duration = argv[++i];
      continue;
    }
    if (std::strcmp(arg, "--path") == 0 && i + 1 < argc) {
      cfg.path = argv[++i];
      if (cfg.path.empty() || cfg.path[0] != '/') {
        cfg.path = "/" + cfg.path;
      }
      continue;
    }
    if (std::strcmp(arg, "--warmup-ms") == 0 && i + 1 < argc) {
      parse_int(argv[++i], cfg.warmup_ms);
      continue;
    }
    if (std::strcmp(arg, "--wrk-arg") == 0 && i + 1 < argc) {
      cfg.wrk_args.emplace_back(argv[++i]);
      continue;
    }

    if (std::strcmp(arg, "--no-wrk") == 0) {
      cfg.run_wrk = false;
      continue;
    }
    if (std::strcmp(arg, "--run-seconds") == 0 && i + 1 < argc) {
      parse_int(argv[++i], cfg.run_seconds);
      continue;
    }

    std::cerr << "Unknown argument: " << arg << std::endl;
    usage(argv[0]);
    std::exit(2);
  }

  return cfg;
}

void handle_signal(int) { g_running.store(false, std::memory_order_release); }

int create_server_socket(int port) {
  int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    std::cerr << "socket() failed: " << std::strerror(errno) << std::endl;
    return -1;
  }

  int yes = 1;
  ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));

  sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  addr.sin_addr.s_addr = INADDR_ANY;

  if (::bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) <
      0) {
    std::cerr << "bind() failed: " << std::strerror(errno) << std::endl;
    ::close(listen_fd);
    return -1;
  }

  if (::listen(listen_fd, 4096) < 0) {
    std::cerr << "listen() failed: " << std::strerror(errno) << std::endl;
    ::close(listen_fd);
    return -1;
  }

  int flags = ::fcntl(listen_fd, F_GETFL, 0);
  if (flags >= 0) {
    ::fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);
  }

  return listen_fd;
}

void accept_connection();

void register_accept_event() {
  if (!g_io_scheduler || !g_running.load(std::memory_order_acquire)) {
    return;
  }
  g_io_scheduler->add_event(g_listen_fd, Channel::kRead, accept_connection);
}

void handle_client(int client_fd) {
  g_stats.fibers_created.fetch_add(1, std::memory_order_relaxed);
  set_hook_enable(true);

  char buf[4096];
  int ret = ::recv(client_fd, buf, sizeof(buf), 0);
  if (ret > 0) {
    g_stats.bytes_received.fetch_add(static_cast<uint64_t>(ret),
                                     std::memory_order_relaxed);
    int send_ret =
        ::send(client_fd, kHttpResponse, std::strlen(kHttpResponse), 0);
    if (send_ret > 0) {
      g_stats.bytes_sent.fetch_add(static_cast<uint64_t>(send_ret),
                                   std::memory_order_relaxed);
      g_stats.requests_handled.fetch_add(1, std::memory_order_relaxed);
    }
  }

  ::close(client_fd);
}

void accept_connection() {
  if (!g_running.load(std::memory_order_acquire)) {
    return;
  }

  set_hook_enable(true);

  sockaddr_in client_addr;
  socklen_t client_len = sizeof(client_addr);
  std::memset(&client_addr, 0, sizeof(client_addr));

  int client_fd = ::accept(
      g_listen_fd, reinterpret_cast<sockaddr *>(&client_addr), &client_len);
  if (client_fd < 0) {
    register_accept_event();
    return;
  }

  g_stats.connections_accepted.fetch_add(1, std::memory_order_relaxed);

  int flags = ::fcntl(client_fd, F_GETFL, 0);
  if (flags >= 0) {
    ::fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
  }

  auto fiber =
      std::make_shared<Fiber>([client_fd]() { handle_client(client_fd); });
  g_io_scheduler->schedule(std::move(fiber));

  register_accept_event();
}

void start_server(const BenchConfig &cfg) {
  g_listen_fd = create_server_socket(cfg.port);
  if (g_listen_fd < 0) {
    std::cerr << "Failed to create server socket" << std::endl;
    std::exit(1);
  }

  g_io_scheduler = std::make_shared<IoScheduler>(cfg.thread_num, "ZBench",
                                                 cfg.use_shared_stack);
  g_io_scheduler->start();

  set_hook_enable(true);
  g_running.store(true, std::memory_order_release);
  g_stats.reset();

  g_io_scheduler->add_event(g_listen_fd, Channel::kRead, accept_connection);

  std::cout << "Server started: port=" << cfg.port
            << ", threads=" << cfg.thread_num
            << ", stack=" << (cfg.use_shared_stack ? "shared" : "independent")
            << std::endl;
}

void stop_server() {
  g_running.store(false, std::memory_order_release);
  g_stats.finish();

  if (g_io_scheduler) {
    g_io_scheduler->stop();
    g_io_scheduler.reset();
  }

  if (g_listen_fd >= 0) {
    ::close(g_listen_fd);
    g_listen_fd = -1;
  }
}

std::string build_url(const BenchConfig &cfg) {
  std::ostringstream oss;
  oss << "http://127.0.0.1:" << cfg.port << cfg.path;
  return oss.str();
}

int run_wrk(const BenchConfig &cfg) {
  std::vector<std::string> args;
  args.emplace_back(cfg.wrk_bin);
  args.emplace_back("-t" + std::to_string(cfg.wrk_threads));
  args.emplace_back("-c" + std::to_string(cfg.wrk_connections));
  args.emplace_back("-d" + cfg.wrk_duration);
  for (const auto &a : cfg.wrk_args) {
    args.emplace_back(a);
  }
  args.emplace_back(build_url(cfg));

  std::vector<char *> argv;
  argv.reserve(args.size() + 1);
  for (auto &s : args) {
    argv.push_back(const_cast<char *>(s.c_str()));
  }
  argv.push_back(nullptr);

  std::cout << "Launching wrk:";
  for (const auto &s : args) {
    std::cout << " " << s;
  }
  std::cout << std::endl;

  pid_t pid = ::fork();
  if (pid == 0) {
    if (cfg.warmup_ms > 0) {
      ::usleep(static_cast<useconds_t>(cfg.warmup_ms) * 1000);
    }
    ::execvp(argv[0], argv.data());
    std::cerr << "execvp(wrk) failed: " << std::strerror(errno) << std::endl;
    _exit(127);
  }

  if (pid < 0) {
    std::cerr << "fork() failed: " << std::strerror(errno) << std::endl;
    return 1;
  }

  int status = 0;
  ::waitpid(pid, &status, 0);
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }
  return 1;
}

void print_summary(const BenchConfig &cfg) {
  const double elapsed = g_stats.elapsed_seconds();
  std::cout << "\n===== Summary =====\n";
  std::cout << "mode=" << (cfg.use_shared_stack ? "shared" : "independent")
            << ", elapsed=" << elapsed << "s\n";
  std::cout << "connections=" << g_stats.connections_accepted.load() << "\n";
  std::cout << "requests=" << g_stats.requests_handled.load() << "\n";
  std::cout << "fibers=" << g_stats.fibers_created.load() << "\n";
  std::cout << "rx_bytes=" << g_stats.bytes_received.load() << "\n";
  std::cout << "tx_bytes=" << g_stats.bytes_sent.load() << "\n";
  if (elapsed > 0) {
    std::cout << "server_rps="
              << (static_cast<double>(g_stats.requests_handled.load()) /
                  elapsed)
              << "\n";
  }
  std::cout << "==============\n";
}

} // namespace

int main(int argc, char **argv) {
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);
  std::signal(SIGPIPE, SIG_IGN);

  zcoroutine::FiberPool::get_instance().init();
  zcoroutine::init_logger(zlog::LogLevel::value::ERROR);

  BenchConfig cfg = parse_args(argc, argv);

  start_server(cfg);

  int wrk_rc = 0;

  if (cfg.run_wrk) {
    wrk_rc = run_wrk(cfg);
    g_running.store(false, std::memory_order_release);
  } else {
    auto start = std::chrono::steady_clock::now();
    while (g_running.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (cfg.run_seconds > 0) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - start)
                .count() >= cfg.run_seconds) {
          break;
        }
      }
    }
  }

  stop_server();
  print_summary(cfg);

  if (cfg.run_wrk) {
    std::cout << "wrk_exit_code=" << wrk_rc << std::endl;
  }

  return wrk_rc;
}
