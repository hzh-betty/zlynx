#include "session_manager.h"
#include "server_error.h"

namespace znet::detail {
SessionManager::SessionManager(const ServerOptions &options,
                               const std::vector<zco::Executor> &workers,
                               const std::atomic<bool> &running)
    : options_(options), workers_(workers), running_(running) {}

bool SessionManager::admit(Socket socket, const TaskFactory &make_task) {
    auto stream = options_.tls ? options_.tls->make_stream(std::move(socket))
                               : make_tcp_stream(std::move(socket));
    if (!stream) {
        report_error(options_.on_error, stream.error());
        return true;
    }
    Error submission_error;
    {
        // Register before submission/handshake. stop can close every accepted
        // descriptor, including queued tasks and TLS waits.
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_)
            return false;
        const auto id = ++next_id_;
        auto executor = workers_[(id - 1) % workers_.size()];
        auto connection = std::make_shared<Connection>(
            std::move(stream).value(), executor, options_.write_timeout);
        auto position =
            sessions_.emplace(id, Session{connection, zco::TaskHandle{}});
        auto submitted = executor.spawn(make_task(connection, id));
        if (submitted) {
            position.first->second.task = std::move(submitted).value();
        } else {
            sessions_.erase(id);
            submission_error =
                runtime_error("submit session", submitted.error());
        }
    }
    if (submission_error) {
        if (running_ && submission_error.code != std::errc::operation_canceled)
            report_error(options_.on_error, submission_error);
        return false;
    }
    return true;
}

void SessionManager::close_connections() {
    std::vector<Connection::ptr> connections;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto &entry : sessions_)
            connections.push_back(entry.second.connection);
    }
    for (auto &connection : connections)
        (void)connection->close();
}

size_t SessionManager::active_connections() const {
    size_t active = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &entry : sessions_) {
        const auto status = entry.second.task.status();
        if (status == zco::TaskStatus::pending ||
            status == zco::TaskStatus::running)
            ++active;
    }
    return active;
}

Result<void> SessionManager::start_reaper(const zco::Executor &executor,
                                          zco::Task task) {
    auto submitted = executor.spawn(std::move(task));
    if (!submitted)
        return runtime_error("start session reaper", submitted.error());
    reaper_task_ = std::move(submitted).value();
    return {};
}

void SessionManager::session_completed(uint64_t id) {
    bool wake;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        completed_sessions_.push_back(id);
        wake = completed_sessions_.size() == 1;
    }
    if (wake)
        completed_event_.signal();
}

void SessionManager::finish_accepting() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        accepting_done_ = true;
    }
    completed_event_.signal();
}

void SessionManager::reap_completed() {
    std::vector<uint64_t> completed;
    while (completed_event_.wait()) {
        bool done;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            completed.swap(completed_sessions_);
            done = accepting_done_;
        }
        for (auto id : completed) {
            zco::TaskHandle task;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                task = sessions_.at(id).task;
            }
            // The notification precedes the task's final return. Keep its
            // registration until execution resources have been released.
            // A canceled wait leaves it for the control-thread join.
            if (!task.join())
                return;
            Session retired;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto found = sessions_.find(id);
                retired = std::move(found->second);
                sessions_.erase(found);
            }
        }
        completed.clear();
        if (done)
            return;
    }
}

void SessionManager::join() {
    if (reaper_task_)
        (void)reaper_task_.join();
    std::unordered_map<uint64_t, Session> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending.swap(sessions_);
        completed_sessions_.clear();
    }
    for (auto &entry : pending) {
        (void)entry.second.connection->close();
        if (entry.second.task)
            (void)entry.second.task.join();
    }
}
} // namespace znet::detail
