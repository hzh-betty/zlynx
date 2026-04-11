#include "http_server_builder.h"
#include "zhttp_logger.h"

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

std::atomic<bool> g_server_running{true};

struct BenchConfig {
    int port = 18080;
    int threads = 4;
    std::string wrk_bin = "wrk";
    int wrk_threads = 4;
    int wrk_connections = 256;
    std::string wrk_duration = "3s";
    int warmup_ms = 500;
    std::string path = "/";
    std::string mode = "all";
    std::vector<std::string> wrk_args;
};

struct WrkResult {
    int exit_code = 0;
    std::string output;
    std::string requests_per_sec;
    std::string transfer_per_sec;
    std::string latency;
};

struct ModeResult {
    std::string mode;
    int port = 0;
    int wrk_exit_code = 0;
    bool server_started = false;
    bool server_exited_cleanly = false;
    std::string server_log;
    WrkResult wrk;
};

void print_usage(const char *prog) {
    std::cout
        << "Usage: " << prog << " [options]\n\n"
        << "Options:\n"
        << "  --mode [independent|shared|all]  Stack mode to test (default "
           "all)\n"
        << "  --port N                         Base port (default 18080)\n"
        << "  --threads N                      Server worker threads (default "
           "4)\n"
        << "  --wrk-bin PATH                   wrk binary/path (default wrk)\n"
        << "  --wrk-threads N                  wrk threads (default 4)\n"
        << "  --wrk-connections N              wrk connections (default 256)\n"
        << "  --wrk-duration STR               wrk duration (default 10s)\n"
        << "  --warmup-ms N                    Delay before wrk (default 500)\n"
        << "  --path STR                       Request path (default /)\n"
        << "  --wrk-arg ARG                    Extra arg forwarded to wrk\n"
        << "  -h, --help                       Show help\n";
}

bool parse_int(const char *text, int &value) {
    if (!text || *text == '\0') {
        return false;
    }
    char *end = nullptr;
    long parsed = std::strtol(text, &end, 10);
    if (!end || *end != '\0') {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
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
        if (std::strcmp(arg, "--mode") == 0 && i + 1 < argc) {
            cfg.mode = argv[++i];
            continue;
        }
        if (std::strcmp(arg, "--port") == 0 && i + 1 < argc) {
            if (!parse_int(argv[++i], cfg.port)) {
                std::cerr << "Invalid --port" << std::endl;
                std::exit(2);
            }
            continue;
        }
        if (std::strcmp(arg, "--threads") == 0 && i + 1 < argc) {
            if (!parse_int(argv[++i], cfg.threads)) {
                std::cerr << "Invalid --threads" << std::endl;
                std::exit(2);
            }
            continue;
        }
        if (std::strcmp(arg, "--wrk-bin") == 0 && i + 1 < argc) {
            cfg.wrk_bin = argv[++i];
            continue;
        }
        if (std::strcmp(arg, "--wrk-threads") == 0 && i + 1 < argc) {
            if (!parse_int(argv[++i], cfg.wrk_threads)) {
                std::cerr << "Invalid --wrk-threads" << std::endl;
                std::exit(2);
            }
            continue;
        }
        if (std::strcmp(arg, "--wrk-connections") == 0 && i + 1 < argc) {
            if (!parse_int(argv[++i], cfg.wrk_connections)) {
                std::cerr << "Invalid --wrk-connections" << std::endl;
                std::exit(2);
            }
            continue;
        }
        if (std::strcmp(arg, "--wrk-duration") == 0 && i + 1 < argc) {
            cfg.wrk_duration = argv[++i];
            continue;
        }
        if (std::strcmp(arg, "--warmup-ms") == 0 && i + 1 < argc) {
            if (!parse_int(argv[++i], cfg.warmup_ms)) {
                std::cerr << "Invalid --warmup-ms" << std::endl;
                std::exit(2);
            }
            continue;
        }
        if (std::strcmp(arg, "--path") == 0 && i + 1 < argc) {
            cfg.path = argv[++i];
            if (cfg.path.empty() || cfg.path[0] != '/') {
                cfg.path = "/" + cfg.path;
            }
            continue;
        }
        if (std::strcmp(arg, "--wrk-arg") == 0 && i + 1 < argc) {
            cfg.wrk_args.emplace_back(argv[++i]);
            continue;
        }

        std::cerr << "Unknown argument: " << arg << std::endl;
        print_usage(argv[0]);
        std::exit(2);
    }

    if (cfg.mode != "all" && cfg.mode != "independent" &&
        cfg.mode != "shared") {
        std::cerr << "Invalid --mode: " << cfg.mode << std::endl;
        std::exit(2);
    }
    return cfg;
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

std::string extract_wrk_latency(const std::string &output) {
    const std::string key = "Latency";
    const size_t pos = output.find(key);
    if (pos == std::string::npos) {
        return "n/a";
    }

    size_t line_end = output.find('\n', pos);
    if (line_end == std::string::npos) {
        line_end = output.size();
    }

    std::string line = trim(output.substr(pos, line_end - pos));
    if (line.size() <= key.size()) {
        return "n/a";
    }
    return trim(line.substr(key.size()));
}

void server_signal_handler(int) { g_server_running.store(false); }

int run_server_process(const BenchConfig &cfg, const std::string &mode,
                       int ready_fd) {
    std::signal(SIGTERM, server_signal_handler);
    std::signal(SIGINT, server_signal_handler);
    std::signal(SIGPIPE, SIG_IGN);

    try {
        zhttp::init_logger(zlog::LogLevel::value::ERROR);

        zhttp::HttpServerBuilder builder;
        builder.listen("127.0.0.1", static_cast<uint16_t>(cfg.port))
            .threads(static_cast<size_t>(cfg.threads))
            .log_level("error")
            .server_name("zhttp-bench");

        if (mode == "shared") {
            builder.use_shared_stack();
        } else {
            builder.use_independent_stack();
        }

        builder.get(cfg.path, [](const zhttp::HttpRequest::ptr &,
                                 zhttp::HttpResponse &resp) {
            resp.status(zhttp::HttpStatus::OK).text("OK");
        });

        auto server = builder.build();
        if (!server->start()) {
            std::cerr << "server start failed" << std::endl;
            char ready = '0';
            (void)::write(ready_fd, &ready, 1);
            return 2;
        }

        char ready = '1';
        (void)::write(ready_fd, &ready, 1);

        while (g_server_running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        server->stop();
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "server exception: " << ex.what() << std::endl;
    } catch (...) {
        std::cerr << "server exception: unknown" << std::endl;
    }

    char ready = '0';
    (void)::write(ready_fd, &ready, 1);
    return 2;
}

bool wait_for_server_ready(int ready_fd, int timeout_ms) {
    pollfd pfd;
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

std::string read_all_fd(int fd) {
    std::string output;
    char buffer[4096];
    while (true) {
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n > 0) {
            output.append(buffer, static_cast<size_t>(n));
            continue;
        }
        break;
    }
    return output;
}

WrkResult run_wrk(const BenchConfig &cfg) {
    WrkResult result;
    int pipefd[2] = {-1, -1};
    if (::pipe(pipefd) != 0) {
        result.exit_code = 1;
        result.output = std::string("pipe failed: ") + std::strerror(errno);
        return result;
    }

    std::vector<std::string> args;
    args.emplace_back(cfg.wrk_bin);
    args.emplace_back("-t" + std::to_string(cfg.wrk_threads));
    args.emplace_back("-c" + std::to_string(cfg.wrk_connections));
    args.emplace_back("-d" + cfg.wrk_duration);
    for (const auto &arg : cfg.wrk_args) {
        args.push_back(arg);
    }

    std::ostringstream url;
    url << "http://127.0.0.1:" << cfg.port << cfg.path;
    args.push_back(url.str());

    std::vector<char *> argv;
    argv.reserve(args.size() + 1);
    for (auto &arg : args) {
        argv.push_back(const_cast<char *>(arg.c_str()));
    }
    argv.push_back(nullptr);

    pid_t pid = ::fork();
    if (pid == 0) {
        ::close(pipefd[0]);
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::dup2(pipefd[1], STDERR_FILENO);
        ::close(pipefd[1]);

        if (cfg.warmup_ms > 0) {
            ::usleep(static_cast<useconds_t>(cfg.warmup_ms) * 1000);
        }

        ::execvp(argv[0], argv.data());
        std::perror("execvp wrk");
        _exit(127);
    }

    ::close(pipefd[1]);

    if (pid < 0) {
        ::close(pipefd[0]);
        result.exit_code = 1;
        result.output = std::string("fork failed: ") + std::strerror(errno);
        return result;
    }

    char buffer[4096];
    while (true) {
        const ssize_t n = ::read(pipefd[0], buffer, sizeof(buffer));
        if (n > 0) {
            result.output.append(buffer, static_cast<size_t>(n));
            continue;
        }
        break;
    }
    ::close(pipefd[0]);

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
    result.latency = extract_wrk_latency(result.output);
    return result;
}

bool terminate_server(pid_t pid, bool &clean_exit) {
    clean_exit = false;
    if (pid <= 0) {
        return false;
    }

    ::kill(pid, SIGTERM);
    for (int i = 0; i < 50; ++i) {
        int status = 0;
        pid_t waited = ::waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            clean_exit = WIFEXITED(status) && WEXITSTATUS(status) == 0;
            return true;
        }
        if (waited < 0 && errno == ECHILD) {
            clean_exit = true;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ::kill(pid, SIGKILL);
    int status = 0;
    (void)::waitpid(pid, &status, 0);
    return false;
}

ModeResult run_mode(const BenchConfig &base_cfg, const std::string &mode,
                    int port) {
    BenchConfig cfg = base_cfg;
    cfg.port = port;

    ModeResult mode_result;
    mode_result.mode = mode;
    mode_result.port = port;

    int ready_pipe[2] = {-1, -1};
    if (::pipe(ready_pipe) != 0) {
        mode_result.wrk_exit_code = 1;
        mode_result.wrk.output =
            std::string("ready pipe failed: ") + std::strerror(errno);
        return mode_result;
    }

    int log_pipe[2] = {-1, -1};
    if (::pipe(log_pipe) != 0) {
        ::close(ready_pipe[0]);
        ::close(ready_pipe[1]);
        mode_result.wrk_exit_code = 1;
        mode_result.wrk.output =
            std::string("log pipe failed: ") + std::strerror(errno);
        return mode_result;
    }

    pid_t server_pid = ::fork();
    if (server_pid == 0) {
        ::close(ready_pipe[0]);
        ::close(log_pipe[0]);
        ::dup2(log_pipe[1], STDOUT_FILENO);
        ::dup2(log_pipe[1], STDERR_FILENO);
        ::close(log_pipe[1]);
        const int rc = run_server_process(cfg, mode, ready_pipe[1]);
        ::close(ready_pipe[1]);
        _exit(rc);
    }

    ::close(ready_pipe[1]);
    ::close(log_pipe[1]);
    if (server_pid < 0) {
        ::close(ready_pipe[0]);
        ::close(log_pipe[0]);
        mode_result.wrk_exit_code = 1;
        mode_result.wrk.output =
            std::string("server fork failed: ") + std::strerror(errno);
        return mode_result;
    }

    mode_result.server_started = wait_for_server_ready(ready_pipe[0], 5000);
    ::close(ready_pipe[0]);

    if (!mode_result.server_started) {
        terminate_server(server_pid, mode_result.server_exited_cleanly);
        mode_result.server_log = read_all_fd(log_pipe[0]);
        ::close(log_pipe[0]);
        mode_result.wrk_exit_code = 1;
        mode_result.wrk.output = "server did not become ready within 5s";
        return mode_result;
    }

    mode_result.wrk = run_wrk(cfg);
    mode_result.wrk_exit_code = mode_result.wrk.exit_code;
    terminate_server(server_pid, mode_result.server_exited_cleanly);
    mode_result.server_log = read_all_fd(log_pipe[0]);
    ::close(log_pipe[0]);
    return mode_result;
}

void print_mode_result(const ModeResult &result) {
    std::cout << "\n===== " << result.mode << " stack =====\n";
    std::cout << "port=" << result.port
              << ", server_started=" << (result.server_started ? "yes" : "no")
              << ", server_exit="
              << (result.server_exited_cleanly ? "clean" : "forced")
              << ", wrk_exit_code=" << result.wrk_exit_code << "\n";
    std::cout << "--- wrk raw output begin ---\n";
    std::cout << result.wrk.output;
    if (!result.wrk.output.empty() && result.wrk.output.back() != '\n') {
        std::cout << '\n';
    }
    std::cout << "--- wrk raw output end ---\n";
    if (!result.server_log.empty()) {
        std::cout << "--- server log begin ---\n";
        std::cout << result.server_log;
        if (result.server_log.back() != '\n') {
            std::cout << '\n';
        }
        std::cout << "--- server log end ---\n";
    }
}

void print_summary(const std::vector<ModeResult> &results) {
    std::cout << "\n===== summary =====\n";
    for (const auto &result : results) {
        std::cout << "mode=" << result.mode << " | port=" << result.port
                  << " | wrk_rc=" << result.wrk_exit_code << " | server="
                  << (result.server_exited_cleanly ? "clean" : "forced")
                  << " | requests/sec=" << result.wrk.requests_per_sec
                  << " | latency=" << result.wrk.latency
                  << " | transfer/sec=" << result.wrk.transfer_per_sec << '\n';
    }
}

} // namespace

int main(int argc, char **argv) {
    std::signal(SIGPIPE, SIG_IGN);

    const BenchConfig cfg = parse_args(argc, argv);
    std::vector<std::string> modes;
    if (cfg.mode == "all") {
        modes.push_back("independent");
        modes.push_back("shared");
    } else {
        modes.push_back(cfg.mode);
    }

    std::vector<ModeResult> results;
    results.reserve(modes.size());

    for (size_t i = 0; i < modes.size(); ++i) {
        const int port = cfg.port + static_cast<int>(i);
        results.push_back(run_mode(cfg, modes[i], port));
        print_mode_result(results.back());
    }

    print_summary(results);

    for (const auto &result : results) {
        if (result.wrk_exit_code != 0 || !result.server_started) {
            return 1;
        }
    }
    return 0;
}