#include "acceptor.h"
#include "server_error.h"

namespace znet::detail {
Acceptor::Acceptor(Socket listener) : listener_(std::move(listener)) {}

Result<Socket> Acceptor::listen(const Endpoint &endpoint, int backlog,
                                bool reuse_port) {
    auto socket = Socket::create(endpoint.family(), SocketKind::stream);
    if (!socket)
        return socket.error();
    auto reuse = socket.value().set_option(SOL_SOCKET, SO_REUSEADDR, 1);
    if (!reuse)
        return reuse.error();
    if (reuse_port) {
        auto configured =
            socket.value().set_option(SOL_SOCKET, SO_REUSEPORT, 1);
        if (!configured)
            return configured.error();
    }
    auto bound = socket.value().bind(endpoint);
    if (!bound)
        return bound.error();
    auto listening = socket.value().listen(backlog);
    if (!listening)
        return listening.error();
    return std::move(socket).value();
}

Result<void> Acceptor::start(const zco::Executor &executor, zco::Task task) {
    auto submitted = executor.spawn(std::move(task));
    if (!submitted)
        return runtime_error("start TCP server", submitted.error());
    task_ = std::move(submitted).value();
    return {};
}

void Acceptor::accept(const std::atomic<bool> &running,
                      const std::function<bool(Socket)> &on_accept,
                      const std::function<void(const Error &)> &on_error) {
    try {
        while (running) {
            auto accepted = listener_.accept();
            if (!accepted) {
                if (running &&
                    accepted.error().code != std::errc::operation_canceled)
                    report_error(on_error, accepted.error());
                break;
            }
            if (!running || !on_accept(std::move(accepted).value()))
                break;
        }
    } catch (const std::exception &error) {
        report_error(on_error, callback_error(error.what()));
    } catch (...) {
        report_error(on_error, callback_error("unknown accept exception"));
    }
}

void Acceptor::request_stop() { (void)listener_.close(); }

void Acceptor::join() {
    if (task_)
        (void)task_.join();
}
} // namespace znet::detail
