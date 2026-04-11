#include "zco/zco_log.h"
#include "znet/address.h"
#include "znet/buffer.h"
#include "znet/tcp_connection.h"
#include "znet/tcp_server.h"
#include "znet/znet_logger.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

std::atomic<bool> g_server_running(true);

struct BenchConfig {
    int port = 18080;
    int server_threads = 4;
    std::string wrk_bin = "wrk";
    int wrk_threads = 4;
    int wrk_connections = 256;
    std::string wrk_duration = "5s";
    int warmup_ms = 300;
    std::string path = "/";
    int scale_pct = 100;
    int server_ready_timeout_ms = 3000;
    int shutdown_timeout_ms = 5000;
    std::vector<std::string> wrk_args;
};

struct WrkResult {
    int exit_code = 0;
    std::string output;
    std::string requests_per_sec = "n/a";
    std::string transfer_per_sec = "n/a";
    std::string latency = "n/a";
};

struct RunResult {
    bool server_started = false;
    bool server_ready = false;
    bool server_exited_cleanly = false;
    int server_exit_code = 0;
    WrkResult wrk;
    std::string server_log;
};

bool parse_int(const char *text, int *value) {
    if (!text || !value || *text == '\0') {
        return false;
    }

    char *end = nullptr;
    errno = 0;
    const long parsed = std::strtol(text, &end, 10);
    if (errno != 0 || end == text || (end && *end != '\0')) {
        return false;
    }

    *value = static_cast<int>(parsed);
    return true;
}

int read_env_int(const char *name, int default_value, int min_value,
                 int max_value) {
    const char *raw = std::getenv(name);
    if (!raw || raw[0] == '\0') {
        return default_value;
    }

    int parsed = default_value;
    if (!parse_int(raw, &parsed)) {
        return default_value;
    }

    if (parsed < min_value || parsed > max_value) {
        return default_value;
    }

    return parsed;
}

int scaled_value(const int base, const int scale_pct, const int min_value) {
    const long long scaled =
        (static_cast<long long>(base) * static_cast<long long>(scale_pct)) /
        100;
    if (scaled < min_value) {
        return min_value;
    }
    return static_cast<int>(scaled);
}

std::string trim(const std::string &text) {
    const char *spaces = " \t\r\n";
    const size_t begin = text.find_first_not_of(spaces);
    if (begin == std::string::npos) {
        return "";
    }
    const size_t end = text.find_last_not_of(spaces);
    return text.substr(begin, end - begin + 1);
}

std::string to_lower_ascii(const std::string &text) {
    std::string lowered = text;
    for (size_t i = 0; i < lowered.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(lowered[i]);
        if (c >= 'A' && c <= 'Z') {
            lowered[i] = static_cast<char>(c - 'A' + 'a');
        }
    }
    return lowered;
}

std::string extract_metric(const std::string &output, const std::string &key) {
    const std::string needle = key + ':';
    const size_t pos = output.find(needle);
    if (pos == std::string::npos) {
        return "n/a";
    }
    size_t line_end = output.find('\n', pos);
    if (line_end == std::string::npos) {
        line_end = output.size();
    }
    return trim(
        output.substr(pos + needle.size(), line_end - pos - needle.size()));
}

std::string extract_latency(const std::string &output) {
    const std::string key = "Latency";
    const size_t pos = output.find(key);
    if (pos == std::string::npos) {
        return "n/a";
    }
    size_t line_end = output.find('\n', pos);
    if (line_end == std::string::npos) {
        line_end = output.size();
    }
    const std::string line = trim(output.substr(pos, line_end - pos));
    if (line.size() <= key.size()) {
        return "n/a";
    }
    return trim(line.substr(key.size()));
}

void print_usage(const char *prog) {
    std::cout
        << "Usage: " << prog << " [options]\n\n"
        << "Options:\n"
        << "  --port N                      Listen port (default 18080)\n"
        << "  --threads N                   Server worker threads (default 4)\n"
        << "  --wrk-bin PATH                wrk binary/path (default wrk)\n"
        << "  --wrk-threads N               wrk threads (default 4)\n"
        << "  --wrk-connections N           wrk connections (default 256)\n"
        << "  --wrk-duration STR            wrk duration (default 5s)\n"
        << "  --warmup-ms N                 Delay before wrk exec (default "
           "300)\n"
        << "  --path STR                    Request path (default /)\n"
        << "  --scale-pct N                 Workload scale percent (default "
           "100)\n"
        << "  --server-ready-timeout-ms N   Server ready timeout (default "
           "3000)\n"
        << "  --shutdown-timeout-ms N       Graceful shutdown timeout (default "
           "5000)\n"
        << "  --wrk-arg ARG                 Extra arg forwarded to wrk "
           "(repeatable)\n"
        << "  -h, --help                    Show help\n";
}

BenchConfig parse_args(int argc, char **argv) {
    BenchConfig cfg;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (!arg) {
            continue;
        }

        if (std::strcmp(arg, "-h") == 0 || std::strcmp(arg, "--help") == 0) {
            print_usage(argv[0]);
            std::exit(0);
        }

        auto require_next = [&](const char *name) -> const char * {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << name << std::endl;
                std::exit(2);
            }
            return argv[++i];
        };

        if (std::strcmp(arg, "--port") == 0) {
            int value = 0;
            if (!parse_int(require_next("--port"), &value) || value <= 0 ||
                value > 65535) {
                std::cerr << "Invalid --port" << std::endl;
                std::exit(2);
            }
            cfg.port = value;
            continue;
        }

        if (std::strcmp(arg, "--threads") == 0) {
            int value = 0;
            if (!parse_int(require_next("--threads"), &value) || value <= 0) {
                std::cerr << "Invalid --threads" << std::endl;
                std::exit(2);
            }
            cfg.server_threads = value;
            continue;
        }

        if (std::strcmp(arg, "--wrk-bin") == 0) {
            cfg.wrk_bin = require_next("--wrk-bin");
            continue;
        }

        if (std::strcmp(arg, "--wrk-threads") == 0) {
            int value = 0;
            if (!parse_int(require_next("--wrk-threads"), &value) ||
                value <= 0) {
                std::cerr << "Invalid --wrk-threads" << std::endl;
                std::exit(2);
            }
            cfg.wrk_threads = value;
            continue;
        }

        if (std::strcmp(arg, "--wrk-connections") == 0) {
            int value = 0;
            if (!parse_int(require_next("--wrk-connections"), &value) ||
                value <= 0) {
                std::cerr << "Invalid --wrk-connections" << std::endl;
                std::exit(2);
            }
            cfg.wrk_connections = value;
            continue;
        }

        if (std::strcmp(arg, "--wrk-duration") == 0) {
            cfg.wrk_duration = require_next("--wrk-duration");
            continue;
        }

        if (std::strcmp(arg, "--warmup-ms") == 0) {
            int value = 0;
            if (!parse_int(require_next("--warmup-ms"), &value) || value < 0) {
                std::cerr << "Invalid --warmup-ms" << std::endl;
                std::exit(2);
            }
            cfg.warmup_ms = value;
            continue;
        }

        if (std::strcmp(arg, "--path") == 0) {
            cfg.path = require_next("--path");
            if (cfg.path.empty() || cfg.path[0] != '/') {
                cfg.path = "/" + cfg.path;
            }
            continue;
        }

        if (std::strcmp(arg, "--scale-pct") == 0) {
            int value = 0;
            if (!parse_int(require_next("--scale-pct"), &value) || value <= 0 ||
                value > 1000) {
                std::cerr << "Invalid --scale-pct" << std::endl;
                std::exit(2);
            }
            cfg.scale_pct = value;
            continue;
        }

        if (std::strcmp(arg, "--server-ready-timeout-ms") == 0) {
            int value = 0;
            if (!parse_int(require_next("--server-ready-timeout-ms"), &value) ||
                value <= 0) {
                std::cerr << "Invalid --server-ready-timeout-ms" << std::endl;
                std::exit(2);
            }
            cfg.server_ready_timeout_ms = value;
            continue;
        }

        if (std::strcmp(arg, "--shutdown-timeout-ms") == 0) {
            int value = 0;
            if (!parse_int(require_next("--shutdown-timeout-ms"), &value) ||
                value <= 0) {
                std::cerr << "Invalid --shutdown-timeout-ms" << std::endl;
                std::exit(2);
            }
            cfg.shutdown_timeout_ms = value;
            continue;
        }

        if (std::strcmp(arg, "--wrk-arg") == 0) {
            cfg.wrk_args.emplace_back(require_next("--wrk-arg"));
            continue;
        }

        std::cerr << "Unknown argument: " << arg << std::endl;
        print_usage(argv[0]);
        std::exit(2);
    }

    return cfg;
}

void apply_scale(BenchConfig *cfg) {
    if (!cfg) {
        return;
    }

    cfg->scale_pct =
        read_env_int("ZNET_PERF_SCALE_PCT", cfg->scale_pct, 1, 1000);
    cfg->server_threads = scaled_value(cfg->server_threads, cfg->scale_pct, 1);
    cfg->wrk_threads = scaled_value(cfg->wrk_threads, cfg->scale_pct, 1);
    cfg->wrk_connections =
        scaled_value(cfg->wrk_connections, cfg->scale_pct, 1);
}

bool find_executable(const std::string &bin, std::string *resolved) {
    if (bin.empty()) {
        return false;
    }

    if (bin.find('/') != std::string::npos) {
        if (::access(bin.c_str(), X_OK) == 0) {
            if (resolved) {
                *resolved = bin;
            }
            return true;
        }
        return false;
    }

    const char *path_env = std::getenv("PATH");
    if (!path_env) {
        return false;
    }

    std::stringstream ss(path_env);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (dir.empty()) {
            dir = ".";
        }
        std::string candidate = dir + "/" + bin;
        if (::access(candidate.c_str(), X_OK) == 0) {
            if (resolved) {
                *resolved = candidate;
            }
            return true;
        }
    }

    return false;
}

bool wait_for_server_ready(const int ready_fd, const int timeout_ms) {
    struct pollfd pfd;
    pfd.fd = ready_fd;
    pfd.events = POLLIN | POLLHUP;
    pfd.revents = 0;

    const int rc = ::poll(&pfd, 1, timeout_ms);
    if (rc <= 0) {
        return false;
    }

    char ready = '0';
    const ssize_t n = ::read(ready_fd, &ready, 1);
    return n == 1 && ready == '1';
}

std::string read_all_fd(const int fd) {
    std::string output;
    char buffer[4096];
    while (true) {
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n <= 0) {
            break;
        }
        output.append(buffer, static_cast<size_t>(n));
    }
    return output;
}

bool terminate_process(pid_t pid, const int timeout_ms, bool *clean_exit,
                       int *exit_code) {
    if (clean_exit) {
        *clean_exit = false;
    }
    if (exit_code) {
        *exit_code = 1;
    }

    if (pid <= 0) {
        return false;
    }

    (void)::kill(pid, SIGTERM);

    const int loops = timeout_ms / 100;
    for (int i = 0; i < loops; ++i) {
        int status = 0;
        const pid_t waited = ::waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            if (WIFEXITED(status)) {
                const int code = WEXITSTATUS(status);
                if (exit_code) {
                    *exit_code = code;
                }
                if (clean_exit) {
                    *clean_exit = (code == 0);
                }
            } else if (WIFSIGNALED(status)) {
                if (exit_code) {
                    *exit_code = 128 + WTERMSIG(status);
                }
            }
            return true;
        }

        if (waited < 0 && errno == ECHILD) {
            if (clean_exit) {
                *clean_exit = true;
            }
            if (exit_code) {
                *exit_code = 0;
            }
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    (void)::kill(pid, SIGKILL);
    int status = 0;
    (void)::waitpid(pid, &status, 0);
    if (WIFSIGNALED(status) && exit_code) {
        *exit_code = 128 + WTERMSIG(status);
    }
    return false;
}

bool request_contains_connection_close(const char *data, size_t length) {
    if (!data || length == 0) {
        return false;
    }

    const std::string lowered = to_lower_ascii(std::string(data, length));
    const size_t pos = lowered.find("connection:");
    if (pos == std::string::npos) {
        return false;
    }

    size_t line_end = lowered.find("\r\n", pos);
    if (line_end == std::string::npos) {
        line_end = lowered.size();
    }

    const std::string line = lowered.substr(pos, line_end - pos);
    return line.find("close") != std::string::npos;
}

void server_signal_handler(int) { g_server_running.store(false); }

void notify_ready(const int ready_fd, const char flag) {
    char value = flag;
    while (true) {
        const ssize_t n = ::write(ready_fd, &value, 1);
        if (n == 1) {
            return;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return;
    }
}

class HelloWorldHandler {
  public:
    void on_message(const znet::TcpConnection::ptr &conn,
                    znet::Buffer &buffer) {
        if (!conn) {
            return;
        }

        static const char kHeaderEnd[] = "\r\n\r\n";
        static const std::string kResponseKeepAlive =
            "HTTP/1.1 200 OK\r\n"
            "Server: znet-wrk-bench\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 11\r\n"
            "Connection: keep-alive\r\n"
            "\r\n"
            "hello world";
        static const std::string kResponseClose = "HTTP/1.1 200 OK\r\n"
                                                  "Server: znet-wrk-bench\r\n"
                                                  "Content-Type: text/plain\r\n"
                                                  "Content-Length: 11\r\n"
                                                  "Connection: close\r\n"
                                                  "\r\n"
                                                  "hello world";

        while (buffer.readable_bytes() > 0) {
            const char *begin = buffer.peek();
            const char *end = buffer.begin_write();
            if (!begin || !end || begin >= end) {
                break;
            }

            const char *header_end =
                std::search(begin, end, kHeaderEnd, kHeaderEnd + 4);
            if (header_end == end) {
                break;
            }

            const size_t request_size =
                static_cast<size_t>((header_end - begin) + 4);
            const bool wants_close =
                request_contains_connection_close(begin, request_size);
            buffer.retrieve(request_size);

            const std::string &response =
                wants_close ? kResponseClose : kResponseKeepAlive;
            if (conn->send(response.data(), response.size()) < 0) {
                conn->close();
                return;
            }

            if (wants_close) {
                conn->shutdown();
                return;
            }
        }
    }
};

int run_server_process(const BenchConfig &cfg, const int ready_fd) {
    std::signal(SIGTERM, server_signal_handler);
    std::signal(SIGINT, server_signal_handler);
    std::signal(SIGPIPE, SIG_IGN);

    zco::init_logger(zlog::LogLevel::value::ERROR);
    znet::init_logger(zlog::LogLevel::value::ERROR);

    auto address = std::make_shared<znet::IPv4Address>(
        "127.0.0.1", static_cast<uint16_t>(cfg.port));
    auto server = std::make_shared<znet::TcpServer>(address, 4096);
    server->set_thread_count(cfg.server_threads);
    server->set_read_timeout(100);
    server->set_write_timeout(1000);

    HelloWorldHandler handler;
    server->set_on_message(
        [&handler](const znet::TcpConnection::ptr &conn, znet::Buffer &buffer) {
            handler.on_message(conn, buffer);
        });

    if (!server->start()) {
        notify_ready(ready_fd, '0');
        return 2;
    }

    notify_ready(ready_fd, '1');

    while (g_server_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    server->stop();
    return 0;
}

WrkResult run_wrk(const BenchConfig &cfg) {
    WrkResult result;

    std::string resolved;
    if (!find_executable(cfg.wrk_bin, &resolved)) {
        result.exit_code = 127;
        result.output =
            "wrk binary not found or not executable: " + cfg.wrk_bin;
        return result;
    }

    int output_pipe[2] = {-1, -1};
    if (::pipe(output_pipe) != 0) {
        result.exit_code = 1;
        result.output = std::string("pipe failed: ") + std::strerror(errno);
        return result;
    }

    std::vector<std::string> args;
    args.emplace_back(resolved);
    args.emplace_back("-t" + std::to_string(cfg.wrk_threads));
    args.emplace_back("-c" + std::to_string(cfg.wrk_connections));
    args.emplace_back("-d" + cfg.wrk_duration);
    for (size_t i = 0; i < cfg.wrk_args.size(); ++i) {
        args.push_back(cfg.wrk_args[i]);
    }

    std::ostringstream url;
    url << "http://127.0.0.1:" << cfg.port << cfg.path;
    args.push_back(url.str());

    std::vector<char *> argv;
    argv.reserve(args.size() + 1);
    for (size_t i = 0; i < args.size(); ++i) {
        argv.push_back(const_cast<char *>(args[i].c_str()));
    }
    argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(output_pipe[0]);
        ::close(output_pipe[1]);
        result.exit_code = 1;
        result.output = std::string("fork failed: ") + std::strerror(errno);
        return result;
    }

    if (pid == 0) {
        ::close(output_pipe[0]);
        (void)::dup2(output_pipe[1], STDOUT_FILENO);
        (void)::dup2(output_pipe[1], STDERR_FILENO);
        ::close(output_pipe[1]);

        if (cfg.warmup_ms > 0) {
            ::usleep(static_cast<useconds_t>(cfg.warmup_ms) * 1000);
        }

        ::execvp(argv[0], argv.data());
        std::perror("execvp wrk");
        _exit(127);
    }

    ::close(output_pipe[1]);
    result.output = read_all_fd(output_pipe[0]);
    ::close(output_pipe[0]);

    int status = 0;
    (void)::waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    } else {
        result.exit_code = 1;
    }

    result.requests_per_sec = extract_metric(result.output, "Requests/sec");
    result.transfer_per_sec = extract_metric(result.output, "Transfer/sec");
    result.latency = extract_latency(result.output);
    return result;
}

RunResult run_once(const BenchConfig &cfg) {
    RunResult result;

    int ready_pipe[2] = {-1, -1};
    int server_log_pipe[2] = {-1, -1};
    if (::pipe(ready_pipe) != 0 || ::pipe(server_log_pipe) != 0) {
        result.wrk.exit_code = 1;
        result.wrk.output = "failed to create control pipes";
        if (ready_pipe[0] >= 0) {
            ::close(ready_pipe[0]);
        }
        if (ready_pipe[1] >= 0) {
            ::close(ready_pipe[1]);
        }
        if (server_log_pipe[0] >= 0) {
            ::close(server_log_pipe[0]);
        }
        if (server_log_pipe[1] >= 0) {
            ::close(server_log_pipe[1]);
        }
        return result;
    }

    const pid_t server_pid = ::fork();
    if (server_pid < 0) {
        ::close(ready_pipe[0]);
        ::close(ready_pipe[1]);
        ::close(server_log_pipe[0]);
        ::close(server_log_pipe[1]);
        result.wrk.exit_code = 1;
        result.wrk.output = std::string("fork failed: ") + std::strerror(errno);
        return result;
    }

    if (server_pid == 0) {
        ::close(ready_pipe[0]);
        ::close(server_log_pipe[0]);

        (void)::dup2(server_log_pipe[1], STDOUT_FILENO);
        (void)::dup2(server_log_pipe[1], STDERR_FILENO);
        ::close(server_log_pipe[1]);

        const int rc = run_server_process(cfg, ready_pipe[1]);
        ::close(ready_pipe[1]);
        _exit(rc);
    }

    result.server_started = true;

    ::close(ready_pipe[1]);
    ::close(server_log_pipe[1]);

    result.server_ready =
        wait_for_server_ready(ready_pipe[0], cfg.server_ready_timeout_ms);
    ::close(ready_pipe[0]);

    if (result.server_ready) {
        result.wrk = run_wrk(cfg);
    } else {
        result.wrk.exit_code = 1;
        result.wrk.output = "server did not become ready within timeout";
    }

    (void)terminate_process(server_pid, cfg.shutdown_timeout_ms,
                            &result.server_exited_cleanly,
                            &result.server_exit_code);
    result.server_log = read_all_fd(server_log_pipe[0]);
    ::close(server_log_pipe[0]);

    return result;
}

void print_config(const BenchConfig &cfg) {
    std::cout << "[znet-wrk-bench] config" << " port=" << cfg.port
              << " server_threads=" << cfg.server_threads
              << " wrk_bin=" << cfg.wrk_bin
              << " wrk_threads=" << cfg.wrk_threads
              << " wrk_connections=" << cfg.wrk_connections
              << " wrk_duration=" << cfg.wrk_duration
              << " warmup_ms=" << cfg.warmup_ms << " path=" << cfg.path
              << " scale_pct=" << cfg.scale_pct << std::endl;
}

void print_summary(const RunResult &result) {
    std::cout << "ZNET_WRK_SUMMARY"
              << " server_started=" << (result.server_started ? 1 : 0)
              << " server_ready=" << (result.server_ready ? 1 : 0)
              << " server_exit_code=" << result.server_exit_code
              << " server_clean_exit=" << (result.server_exited_cleanly ? 1 : 0)
              << " wrk_exit_code=" << result.wrk.exit_code
              << " requests_per_sec=\"" << result.wrk.requests_per_sec << "\""
              << " transfer_per_sec=\"" << result.wrk.transfer_per_sec << "\""
              << " latency=\"" << result.wrk.latency << "\"" << std::endl;

    std::cout << "ZNET_WRK_OUTPUT_BEGIN" << std::endl;
    std::cout << result.wrk.output << std::endl;
    std::cout << "ZNET_WRK_OUTPUT_END" << std::endl;

    if (!result.server_log.empty()) {
        std::cout << "ZNET_SERVER_LOG_BEGIN" << std::endl;
        std::cout << result.server_log << std::endl;
        std::cout << "ZNET_SERVER_LOG_END" << std::endl;
    }
}

} // namespace

int main(int argc, char **argv) {
    BenchConfig cfg = parse_args(argc, argv);
    apply_scale(&cfg);
    print_config(cfg);

    RunResult result = run_once(cfg);
    print_summary(result);

    if (!result.server_started || !result.server_ready) {
        return 3;
    }
    if (result.wrk.exit_code != 0) {
        return result.wrk.exit_code;
    }
    return 0;
}
