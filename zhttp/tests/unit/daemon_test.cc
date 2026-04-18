#include "zhttp/daemon.h"
#include "zhttp/zhttp_logger.h"

#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <cerrno>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace zhttp {
namespace {

bool wait_for_file_content(const std::string &path, std::string *content) {
    constexpr int kMaxRetry = 200;
    for (int i = 0; i < kMaxRetry; ++i) {
        std::ifstream input(path);
        if (input.good()) {
            std::string data;
            std::getline(input, data);
            if (!data.empty()) {
                if (content) {
                    *content = data;
                }
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

int run_daemon_helper_mode(const std::string &marker_path) {
    char arg0[] = "daemon_helper";
    char *argv[] = {arg0, nullptr};
    return Daemon::start_daemon(
        1, argv,
        [&marker_path](int, char **) {
            std::ofstream output(marker_path, std::ios::trunc);
            output << "daemon-worker-ran";
            output.close();
            return 0;
        },
        true, 0);
}

int run_daemon_signal_helper_mode(const std::string &pid_path) {
    char arg0[] = "daemon_signal_helper";
    char *argv[] = {arg0, nullptr};
    return Daemon::start_daemon(
        1, argv,
        [&pid_path](int, char **) {
            std::ofstream output(pid_path, std::ios::trunc);
            output << getppid();
            output.close();
            ::sleep(10);
            return 0;
        },
        true, 0);
}

TEST(DaemonTest, ProcessInfoToStringContainsAllFields) {
    ProcessInfo info;
    info.parent_id = 11;
    info.main_id = 22;
    info.parent_start_time = 33;
    info.main_start_time = 44;
    info.restart_count = 55;

    const std::string text = info.to_string();
    EXPECT_NE(text.find("parent_id=11"), std::string::npos);
    EXPECT_NE(text.find("main_id=22"), std::string::npos);
    EXPECT_NE(text.find("parent_start_time=33"), std::string::npos);
    EXPECT_NE(text.find("main_start_time=44"), std::string::npos);
    EXPECT_NE(text.find("restart_count=55"), std::string::npos);
}

TEST(DaemonTest, ForegroundStartRunsMainCallbackAndSetsProcessInfo) {
    bool called = false;
    int observed_argc = -1;
    char arg0[] = "daemon_test";
    char *argv[] = {arg0, nullptr};

    const int rc = Daemon::start_daemon(
        1, argv,
        [&called, &observed_argc](int argc, char **input_argv) {
            called = true;
            observed_argc = argc;
            EXPECT_NE(input_argv, nullptr);
            return 7;
        },
        false);

    EXPECT_EQ(rc, 7);
    EXPECT_TRUE(called);
    EXPECT_EQ(observed_argc, 1);
    EXPECT_FALSE(Daemon::should_stop());

    const ProcessInfo &info = ProcessInfo::instance();
    EXPECT_EQ(info.main_id, getpid());
    EXPECT_GT(info.main_start_time, 0u);
}

TEST(DaemonTest, ReturnsErrorWhenMainCallbackIsEmpty) {
    const int rc = Daemon::start_daemon(0, nullptr, Daemon::MainCallback{},
                                        false);
    EXPECT_EQ(rc, -1);
}

TEST(DaemonTest, SignalHandlersSetStopFlagForSigtermAndSigint) {
    ASSERT_EQ(Daemon::start_daemon(
                  0, nullptr, [](int, char **) { return 0; }, false),
              0);
    ASSERT_FALSE(Daemon::should_stop());

    Daemon::setup_signal_handlers();
    std::raise(SIGTERM);
    EXPECT_TRUE(Daemon::should_stop());

    ASSERT_EQ(Daemon::start_daemon(
                  0, nullptr, [](int, char **) { return 0; }, false),
              0);
    ASSERT_FALSE(Daemon::should_stop());

    Daemon::setup_signal_handlers();
    std::raise(SIGINT);
    EXPECT_TRUE(Daemon::should_stop());
}

TEST(DaemonTest, DaemonModeRunsWorkerCallbackInSubprocess) {
    char marker_template[] = "/tmp/zhttp-daemon-marker-XXXXXX";
    const int marker_fd = ::mkstemp(marker_template);
    ASSERT_GE(marker_fd, 0);
    ::close(marker_fd);
    ::unlink(marker_template);

    pid_t child = ::fork();
    ASSERT_GE(child, 0);

    if (child == 0) {
        ::execl("/proc/self/exe", "/proc/self/exe", "--daemon-helper",
                marker_template, nullptr);
        _exit(127);
    }

    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);

    std::string content;
    EXPECT_TRUE(wait_for_file_content(marker_template, &content));
    EXPECT_EQ(content, "daemon-worker-ran");

    ::unlink(marker_template);
}

TEST(DaemonTest, DaemonModeCanBeStoppedBySignalWhileWaitingChild) {
    char pid_template[] = "/tmp/zhttp-daemon-pid-XXXXXX";
    const int pid_fd = ::mkstemp(pid_template);
    ASSERT_GE(pid_fd, 0);
    ::close(pid_fd);
    ::unlink(pid_template);

    pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        ::execl("/proc/self/exe", "/proc/self/exe", "--daemon-signal-helper",
                pid_template, nullptr);
        _exit(127);
    }

    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);

    std::string daemon_parent_pid_text;
    ASSERT_TRUE(wait_for_file_content(pid_template, &daemon_parent_pid_text));
    const pid_t daemon_parent_pid =
        static_cast<pid_t>(std::strtol(daemon_parent_pid_text.c_str(), nullptr, 10));
    ASSERT_GT(daemon_parent_pid, 1);

    ASSERT_EQ(::kill(daemon_parent_pid, SIGTERM), 0);

    bool exited = false;
    for (int i = 0; i < 200; ++i) {
        if (::kill(daemon_parent_pid, 0) != 0 && errno == ESRCH) {
            exited = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(exited);

    ::unlink(pid_template);
}

} // namespace
} // namespace zhttp

int main(int argc, char **argv) {
    if (argc >= 3 && std::string(argv[1]) == "--daemon-helper") {
        return zhttp::run_daemon_helper_mode(argv[2]);
    }
    if (argc >= 3 && std::string(argv[1]) == "--daemon-signal-helper") {
        return zhttp::run_daemon_signal_helper_mode(argv[2]);
    }

    zhttp::init_logger();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
