#pragma once

#include "zco/runtime.h"
#include "znet/transport/socket.h"
#include <atomic>
#include <functional>

namespace znet::detail {
class Acceptor {
  public:
    explicit Acceptor(Socket listener);
    static Result<Socket> listen(const Endpoint &endpoint, int backlog,
                                 bool reuse_port);

    // The submitted task retains the owning ServerRun and calls accept().
    Result<void> start(const zco::Executor &executor, zco::Task task);
    void accept(const std::atomic<bool> &running,
                const std::function<bool(Socket)> &on_accept,
                const std::function<void(const Error &)> &on_error);
    void request_stop();
    void join();

  private:
    Socket listener_;
    zco::TaskHandle task_;
};
} // namespace znet::detail
