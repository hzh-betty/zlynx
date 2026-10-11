#include "znet/server/tcp_server.h"
#include "acceptor.h"
#include "session_manager.h"
#include "session_runner.h"
#include "zco/coroutine.h"
#include <atomic>
#include <vector>

namespace znet {
namespace {
// Tasks retain only their run, never a facade/Runtime/application raw pointer.
// Each restart creates a fresh run and fresh session ids.
struct ServerRun {
    ServerRun(Socket socket, Endpoint bound, SessionFactory create_session,
              ServerOptions config, std::vector<zco::Executor> executors)
        : endpoint(std::move(bound)), factory(std::move(create_session)),
          options(std::move(config)), workers(std::move(executors)),
          acceptor(std::move(socket)), sessions(options, workers, running) {}

    const Endpoint endpoint;
    const SessionFactory factory;
    const ServerOptions options;
    const std::vector<zco::Executor> workers;
    std::atomic<bool> running{true};
    detail::Acceptor acceptor;
    detail::SessionManager sessions;

    Result<void> start_tasks(const std::shared_ptr<ServerRun> &self) {
        try {
            auto reaper = sessions.start_reaper(
                workers.front(), [self] { self->sessions.reap_completed(); });
            if (!reaper) {
                rollback_start();
                return reaper.error();
            }
            auto accepting = acceptor.start(workers.front(), [self] {
                self->acceptor.accept(
                    self->running,
                    [&self](Socket socket) {
                        return self->sessions.admit(
                            std::move(socket),
                            [&self](const Connection::ptr &connection,
                                    uint64_t id) -> zco::Task {
                                return [self, connection, id] {
                                    detail::serve_session(
                                        connection, self->factory,
                                        self->options, self->running);
                                    self->sessions.session_completed(id);
                                };
                            });
                    },
                    self->options.on_error);
                self->request_stop();
                self->sessions.finish_accepting();
            });
            if (!accepting) {
                rollback_start();
                return accepting.error();
            }
        } catch (...) {
            rollback_start();
            throw;
        }
        return {};
    }

    void request_stop() {
        running.store(false);
        acceptor.request_stop();
        sessions.close_connections();
    }

    void join() {
        acceptor.join();
        sessions.join();
    }

    void rollback_start() {
        request_stop();
        // Retire the reaper even when no accept task could be submitted.
        sessions.finish_accepting();
        join();
    }
};
} // namespace

struct TcpServer::Impl {
    Impl(zco::Runtime &rt, Endpoint listen, SessionFactory create,
         ServerOptions config)
        : runtime(rt), endpoint(std::move(listen)), factory(std::move(create)),
          options(std::move(config)) {}

    zco::Runtime &runtime;
    const Endpoint endpoint;
    const SessionFactory factory;
    const ServerOptions options;
    std::mutex lifecycle_mutex;
    std::shared_ptr<ServerRun> run;
};

TcpServer::TcpServer(zco::Runtime &runtime, Endpoint endpoint,
                     SessionFactory factory, ServerOptions options)
    : impl_(std::make_unique<Impl>(runtime, std::move(endpoint),
                                   std::move(factory), std::move(options))) {
    const auto &config = impl_->options;
    if (!impl_->factory || config.backlog <= 0 || !config.read_chunk_size ||
        config.read_timeout.count() < 0 || config.write_timeout.count() < 0 ||
        config.idle_timeout.count() < 0 || config.handshake_timeout.count() < 0)
        throw std::invalid_argument("Invalid TCP server configuration");
}

TcpServer::~TcpServer() {
    if (zco::in_coroutine())
        request_stop(); // Tasks own their run; no dangling facade capture.
    else
        stop();
}

Result<void> TcpServer::start() {
    if (zco::in_coroutine())
        throw std::logic_error("Start the server from a control thread");
    std::lock_guard<std::mutex> lock(impl_->lifecycle_mutex);
    auto previous = std::atomic_load(&impl_->run);
    if (previous && previous->running)
        return {};
    if (previous) {
        previous->request_stop();
        previous->join();
        std::atomic_store(&impl_->run, std::shared_ptr<ServerRun>{});
    }
    auto socket = detail::Acceptor::listen(
        impl_->endpoint, impl_->options.backlog, impl_->options.reuse_port);
    if (!socket)
        return socket.error();
    auto endpoint = socket.value().local_endpoint();
    if (!endpoint)
        return endpoint.error();
    std::vector<zco::Executor> workers;
    for (size_t i = 0; i < impl_->runtime.worker_count(); ++i)
        workers.push_back(impl_->runtime.executor(i));
    auto run = std::make_shared<ServerRun>(
        std::move(socket).value(), std::move(endpoint).value(), impl_->factory,
        impl_->options, std::move(workers));
    // Publish before submission; a callback can immediately request_stop.
    std::atomic_store(&impl_->run, run);
    try {
        auto started = run->start_tasks(run);
        if (!started)
            std::atomic_store(&impl_->run, std::shared_ptr<ServerRun>{});
        return started;
    } catch (...) {
        std::atomic_store(&impl_->run, std::shared_ptr<ServerRun>{});
        throw;
    }
}

void TcpServer::request_stop() {
    if (auto run = std::atomic_load(&impl_->run))
        run->request_stop();
}

void TcpServer::stop() {
    if (zco::in_coroutine())
        throw std::logic_error(
            "Use request_stop from a callback; stop joins all tasks");
    std::lock_guard<std::mutex> lock(impl_->lifecycle_mutex);
    if (auto run = std::atomic_load(&impl_->run)) {
        run->request_stop();
        run->join();
    }
}

bool TcpServer::is_running() const {
    auto run = std::atomic_load(&impl_->run);
    return run && run->running;
}

Result<Endpoint> TcpServer::local_endpoint() const {
    if (auto run = std::atomic_load(&impl_->run))
        return run->endpoint;
    return make_error(ErrorKind::runtime, std::errc::not_connected,
                      "server endpoint");
}

size_t TcpServer::active_connections() const {
    auto run = std::atomic_load(&impl_->run);
    return run ? run->sessions.active_connections() : 0;
}
} // namespace znet
