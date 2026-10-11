#pragma once
#include "process.h"
#include "server_fixture.h"
#include "zhttp/http_server_builder.h"
#include <csignal>
#include <cstring>
#include <memory>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

namespace zhttp::test {
inline std::string test_executable;

inline int run_signal_server(uint16_t port) {
    HttpServerBuilder builder;
    builder.listen("127.0.0.1", port)
        .threads(1)
        .log_level("error")
        .daemon(false)
        .get("/secure", [](HttpContext &context) {
            context.response()
                .status(HttpStatus::OK)
                .text("secure:" + std::to_string(::getpid()));
        });
    try {
        builder.run();
        return 0;
    } catch (const std::exception &error) {
        // Retry only a port reservation lost to another process.
        return std::string(error.what()).find("Address already in use") !=
                       std::string::npos
                   ? 98
                   : 1;
    }
}

inline bool is_signal_helper(int argc, char **argv) {
    return argc == 3 && std::strcmp(argv[1], "--zlynx-signal-server") == 0;
}

struct SignalServer {
    std::unique_ptr<ScopedChild> child;
    std::string response;
};

inline SignalServer start_signal_server() {
    // Builder requires a nonzero configured port. A lost reservation is retried
    // explicitly; a full response identifies the child that actually bound it.
    const auto overall = zco::Deadline::after(std::chrono::seconds(3));
    for (int attempt = 0;
         attempt < 8 && !overall.expired(zco::Deadline::Clock::now());
         ++attempt) {
        auto reservation =
            znet::Socket::create(AF_INET, znet::SocketKind::stream).value();
        reservation.bind(znet::Endpoint::ipv4("127.0.0.1", 0).value()).value();
        const auto endpoint = reservation.local_endpoint().value();
        std::string port = std::to_string(endpoint.port());
        reservation.close().value();
        char mode[] = "--zlynx-signal-server";
        char *args[] = {test_executable.data(), mode, port.data(), nullptr};
        pid_t pid;
        const int error = ::posix_spawn(&pid, test_executable.c_str(), nullptr,
                                        nullptr, args, ::environ);
        if (error)
            throw std::system_error(error, std::generic_category(),
                                    "posix_spawn");
        auto child = std::make_unique<ScopedChild>(pid);
        for (;;) {
            if (child->exited()) {
                if (WIFEXITED(child->status()) &&
                    WEXITSTATUS(child->status()) == 98)
                    break;
                throw std::runtime_error(
                    "signal server exited before becoming ready");
            }
            auto client = connect_to(endpoint, overall);
            if (client && send_all(client.value().native_handle(),
                                   "GET /secure HTTP/1.1\r\nHost: "
                                   "localhost\r\nConnection: close\r\n\r\n")) {
                auto response = SocketReader(client.value().native_handle())
                                    .until_closed(overall);
                if (response &&
                    response.value().find("secure:" + std::to_string(pid)) !=
                        std::string::npos)
                    return {std::move(child), std::move(response).value()};
            }
            if (overall.expired(zco::Deadline::Clock::now()))
                throw std::runtime_error("signal server readiness timed out");
            ::poll(nullptr, 0, 5);
        }
    }
    throw std::runtime_error("signal server could not reserve a port");
}
} // namespace zhttp::test
