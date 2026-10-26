#include "znet/server/tcp_server.h"
#include "acceptor.h"
#include "session_manager.h"
#include "session_runner.h"
#include "zco/coroutine.h"
#include <atomic>
#include <vector>

namespace znet {
namespace {
/**
 * @brief 任务所需状态的寿命与 TcpServer 对象本身的寿命分开。
 * 例如，业务回调中可能销毁 TcpServer。此时当前会话任务还在执行，
 * 不能因为服务器外观对象销毁，就让任务访问的配置和管理器一起失效。
 */
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
            // 先提交清理任务，再提交接收连接任务，避免接收循环结束后没有清理任务。
            auto reaper = sessions.start_reaper(
                workers.front(), [self] { self->sessions.reap_completed(); });
            if (!reaper) {
                rollback_start();
                return reaper.error();
            }

            // 提交接收连接任务；在循环中提交会话任务，直到停止或发生错误。
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
                
                // 接收循环结束后，通知清理任务退出；清理任务会等待所有会话任务完成。
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

        // 即使接收连接任务提交失败，也要结束清理任务。
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
        request_stop(); // 任务持有运行状态，避免外观对象析构后留下悬空捕获。
    else
        stop();
}

Result<void> TcpServer::start() {
    if (zco::in_coroutine())
        throw std::logic_error("Start the server from a control thread");
    std::lock_guard<std::mutex> lock(impl_->lifecycle_mutex);

    // 如果服务器已经在运行，则直接返回。
    auto previous = std::atomic_load(&impl_->run);
    if (previous && previous->running)
        return {};
    
    // 如果之前有运行的服务器，则停止它。
    if (previous) {
        previous->request_stop();
        previous->join();
        std::atomic_store(&impl_->run, std::shared_ptr<ServerRun>{});
    }

    // 创建监听套接字并绑定到指定的端点。
    auto socket = detail::Acceptor::listen(
        impl_->endpoint, impl_->options.backlog, impl_->options.reuse_port);
    if (!socket)
        return socket.error();
    auto endpoint = socket.value().local_endpoint();
    if (!endpoint)
        return endpoint.error();

    // 收集工作执行器，创建本次启动对应的 ServerRun。
    std::vector<zco::Executor> workers;
    for (size_t i = 0; i < impl_->runtime.worker_count(); ++i)
        workers.push_back(impl_->runtime.executor(i));
    auto run = std::make_shared<ServerRun>(
        std::move(socket).value(), std::move(endpoint).value(), impl_->factory,
        impl_->options, std::move(workers));

    // 先发布运行状态再提交任务，使立即执行的回调能调用 request_stop。
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
    // 在协程中调用 stop 会导致死锁，因为 stop 会等待所有任务完成，而当前协程可能是其中之一。
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
