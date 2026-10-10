#include "znet/server/tcp_server.h"
#include "zco/coroutine.h"
#include "zco/sync/event.h"
#include "znet/transport/socket.h"
#include <atomic>
#include <unordered_map>
#include <vector>

namespace znet {
namespace {
using Clock = zco::Deadline::Clock;

struct Session {
    Connection::ptr connection;
    zco::TaskHandle task;
};

// Tasks retain only their run, never a facade/Runtime/application raw pointer.
// Each restart creates a fresh run and fresh session ids.
struct ServerRun {
    ServerRun(Socket socket, Endpoint bound, SessionFactory create_session,
              ServerOptions config, std::vector<zco::Executor> executors)
        : listener(std::move(socket)), endpoint(std::move(bound)),
          factory(std::move(create_session)), options(std::move(config)),
          workers(std::move(executors)) {}

    Socket listener;
    const Endpoint endpoint;
    const SessionFactory factory;
    const ServerOptions options;
    const std::vector<zco::Executor> workers;
    std::atomic<bool> running{true};
    zco::TaskHandle accept_task;
    zco::TaskHandle reaper_task;
    zco::Event completed_event;
    mutable std::mutex sessions_mutex;
    std::unordered_map<uint64_t, Session> sessions;
    std::vector<uint64_t> completed_sessions;
    bool accepting_done = false;
    uint64_t next_id = 0;

    void report(const Error &error) const noexcept {
        if (options.on_error) {
            try {
                options.on_error(error);
            } catch (...) {
            }
        }
    }

    void request_stop() {
        running.store(false);
        (void)listener.close();
        std::vector<Connection::ptr> connections;
        {
            std::lock_guard<std::mutex> lock(sessions_mutex);
            for (auto &entry : sessions)
                connections.push_back(entry.second.connection);
        }
        for (auto &connection : connections)
            (void)connection->close();
    }

    void join() {
        if (accept_task)
            (void)accept_task.join();
        if (reaper_task)
            (void)reaper_task.join();
        std::unordered_map<uint64_t, Session> pending;
        {
            std::lock_guard<std::mutex> lock(sessions_mutex);
            pending.swap(sessions);
            completed_sessions.clear();
        }
        for (auto &entry : pending) {
            (void)entry.second.connection->close();
            if (entry.second.task)
                (void)entry.second.task.join();
        }
    }

    void session_completed(uint64_t id) {
        bool wake;
        {
            std::lock_guard<std::mutex> lock(sessions_mutex);
            completed_sessions.push_back(id);
            wake = completed_sessions.size() == 1;
        }
        if (wake)
            completed_event.signal();
    }

    void finish_accepting() {
        {
            std::lock_guard<std::mutex> lock(sessions_mutex);
            accepting_done = true;
        }
        completed_event.signal();
    }

    void reap_completed() {
        std::vector<uint64_t> completed;
        while (completed_event.wait()) {
            bool done;
            {
                std::lock_guard<std::mutex> lock(sessions_mutex);
                completed.swap(completed_sessions);
                done = accepting_done;
            }
            for (auto id : completed) {
                zco::TaskHandle task;
                {
                    std::lock_guard<std::mutex> lock(sessions_mutex);
                    task = sessions.at(id).task;
                }
                // The notification precedes the task's final return. Keep its
                // registration until execution resources have been released.
                // A canceled wait leaves it for the control-thread join.
                if (!task.join())
                    return;
                Session retired;
                {
                    std::lock_guard<std::mutex> lock(sessions_mutex);
                    auto found = sessions.find(id);
                    retired = std::move(found->second);
                    sessions.erase(found);
                }
            }
            completed.clear();
            if (done)
                return;
        }
    }
};

Error callback_error(std::string detail) {
    return make_error(ErrorKind::application, std::errc::io_error,
                      "session callback", std::move(detail));
}

void serve_session(const std::shared_ptr<ServerRun> &run,
                   const Connection::ptr &connection) {
    SessionCallbacks callbacks;
    bool established = false;
    try {
        const auto handshake =
            run->options.handshake_timeout.count()
                ? zco::Deadline::after(run->options.handshake_timeout)
                : zco::Deadline{};
        auto started = connection->start(handshake);
        if (!started) {
            if (run->running &&
                started.error().code != std::errc::operation_canceled)
                run->report(started.error());
        } else if (run->running) {
            callbacks = run->factory(connection);
            established = true;
            ByteBuffer input;
            auto idle = run->options.idle_timeout.count()
                            ? zco::Deadline::after(run->options.idle_timeout)
                            : zco::Deadline{};
            while (run->running && connection->connected()) {
                auto deadline =
                    run->options.read_timeout.count()
                        ? zco::Deadline::after(run->options.read_timeout)
                        : zco::Deadline{};
                if (idle.time() < deadline.time())
                    deadline = idle;
                auto received = connection->read(
                    input, run->options.read_chunk_size, deadline);
                if (received.bytes) {
                    idle = run->options.idle_timeout.count()
                               ? zco::Deadline::after(run->options.idle_timeout)
                               : zco::Deadline{};
                    if (callbacks.on_message)
                        callbacks.on_message(connection, input);
                    else
                        input.retrieve_all();
                }
                if (received.eof)
                    break;
                if (received.error) {
                    if (!run->running ||
                        connection->state() == Connection::State::closed ||
                        received.error.code == std::errc::operation_canceled)
                        break;
                    if (received.error.code == std::errc::timed_out) {
                        if (idle.expired(Clock::now()) ||
                            connection->read_deadline_expired())
                            break;
                        continue;
                    }
                    // Every other transport error is terminal; no busy-loop
                    // retry or duplicate logging by the lower layers.
                    run->report(received.error);
                    break;
                }
                if (!received.bytes)
                    throw std::logic_error(
                        "Transport read made no progress without EOF");
            }
        }
    } catch (const std::exception &error) {
        run->report(callback_error(error.what()));
    } catch (...) {
        run->report(callback_error("unknown exception"));
    }
    (void)connection->close();
    if (established && callbacks.on_close) {
        try {
            callbacks.on_close(connection);
        } catch (const std::exception &error) {
            run->report(callback_error(error.what()));
        } catch (...) {
            run->report(callback_error("unknown close exception"));
        }
    }
}

void accept_connections(const std::shared_ptr<ServerRun> &run) {
    try {
        while (run->running) {
            auto accepted = run->listener.accept();
            if (!accepted) {
                if (run->running &&
                    accepted.error().code != std::errc::operation_canceled)
                    run->report(accepted.error());
                break;
            }
            if (!run->running)
                break;
            auto stream =
                run->options.tls
                    ? run->options.tls->make_stream(std::move(accepted).value())
                    : make_tcp_stream(std::move(accepted).value());
            if (!stream) {
                run->report(stream.error());
                continue;
            }
            const auto id = ++run->next_id;
            auto executor = run->workers[(id - 1) % run->workers.size()];
            auto connection = std::make_shared<Connection>(
                std::move(stream).value(), executor,
                run->options.write_timeout);
            Error submission_error;
            {
                // Register before submission/handshake. stop can close every
                // accepted descriptor, including queued tasks and TLS waits.
                std::lock_guard<std::mutex> lock(run->sessions_mutex);
                if (!run->running)
                    break;
                auto position = run->sessions.emplace(
                    id, Session{connection, zco::TaskHandle{}});
                auto submitted = executor.spawn(
                    [run, connection, id] {
                        serve_session(run, connection);
                        run->session_completed(id);
                    });
                if (submitted) {
                    position.first->second.task = std::move(submitted).value();
                } else {
                    run->sessions.erase(id);
                    submission_error =
                        runtime_error("submit session", submitted.error());
                }
            }
            if (submission_error) {
                if (run->running &&
                    submission_error.code != std::errc::operation_canceled)
                    run->report(submission_error);
                break;
            }
        }
    } catch (const std::exception &error) {
        run->report(callback_error(error.what()));
    } catch (...) {
        run->report(callback_error("unknown accept exception"));
    }
    run->request_stop();
    run->finish_accepting();
}
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
    auto socket = Socket::create(impl_->endpoint.family(), SocketKind::stream);
    if (!socket)
        return socket.error();
    auto reuse = socket.value().set_option(SOL_SOCKET, SO_REUSEADDR, 1);
    if (!reuse)
        return reuse.error();
    if (impl_->options.reuse_port) {
        auto configured =
            socket.value().set_option(SOL_SOCKET, SO_REUSEPORT, 1);
        if (!configured)
            return configured.error();
    }
    auto bound = socket.value().bind(impl_->endpoint);
    if (!bound)
        return bound.error();
    auto listening = socket.value().listen(impl_->options.backlog);
    if (!listening)
        return listening.error();
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
    auto reaper = run->workers.front().spawn([run] { run->reap_completed(); });
    if (!reaper) {
        run->request_stop();
        std::atomic_store(&impl_->run, std::shared_ptr<ServerRun>{});
        return runtime_error("start session reaper", reaper.error());
    }
    run->reaper_task = std::move(reaper).value();
    auto task = [&] {
        try {
            return run->workers.front().spawn([run] { accept_connections(run); });
        } catch (...) {
            // A submission exception must also retire the waiting reaper.
            run->request_stop();
            run->finish_accepting();
            run->join();
            std::atomic_store(&impl_->run, std::shared_ptr<ServerRun>{});
            throw;
        }
    }();
    if (!task) {
        run->request_stop();
        run->finish_accepting();
        run->join();
        std::atomic_store(&impl_->run, std::shared_ptr<ServerRun>{});
        return runtime_error("start TCP server", task.error());
    }
    run->accept_task = std::move(task).value();
    return {};
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
    if (!run)
        return 0;
    size_t active = 0;
    std::lock_guard<std::mutex> lock(run->sessions_mutex);
    for (const auto &entry : run->sessions) {
        const auto status = entry.second.task.status();
        if (status == zco::TaskStatus::pending ||
            status == zco::TaskStatus::running)
            ++active;
    }
    return active;
}
} // namespace znet
