#pragma once

#include "zco/sync/event.h"
#include "znet/server/tcp_server.h"
#include "znet/transport/socket.h"
#include <atomic>
#include <unordered_map>
#include <vector>

namespace znet::detail {
class SessionManager {
  public:
    using TaskFactory =
        std::function<zco::Task(const Connection::ptr &, uint64_t)>;

    // Configuration and stop state belong to the containing ServerRun.
    SessionManager(const ServerOptions &options,
                   const std::vector<zco::Executor> &workers,
                   const std::atomic<bool> &running);
    // make_task only builds a task; application callbacks execute after spawn.
    bool admit(Socket socket, const TaskFactory &make_task);
    void close_connections();
    size_t active_connections() const;

    // Submitted tasks retain ServerRun, keeping this manager alive.
    Result<void> start_reaper(const zco::Executor &executor, zco::Task task);
    void session_completed(uint64_t id);
    void finish_accepting();
    void reap_completed();
    void join();

  private:
    struct Session {
        Connection::ptr connection;
        zco::TaskHandle task;
    };

    const ServerOptions &options_;
    const std::vector<zco::Executor> &workers_;
    const std::atomic<bool> &running_;
    zco::TaskHandle reaper_task_;
    zco::Event completed_event_;
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, Session> sessions_;
    std::vector<uint64_t> completed_sessions_;
    bool accepting_done_ = false;
    uint64_t next_id_ = 0;
};
} // namespace znet::detail
