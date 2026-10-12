#ifndef ZLYNX_ZNET_SRC_SERVER_SESSION_MANAGER_H_
#define ZLYNX_ZNET_SRC_SERVER_SESSION_MANAGER_H_

#include "zco/sync/event.h"
#include "znet/server/tcp_server.h"
#include "znet/transport/socket.h"
#include <atomic>
#include <unordered_map>
#include <vector>

namespace znet::detail {
/**
 * @brief 登记已接收连接和会话任务，协调停止关闭与任务完成回收。
 *
 * 配置、执行器和运行标志均借用所属 ServerRun，须覆盖管理器及任务寿命。
 * mutex_ 保护会话表和完成通知；应用回调由提交后的会话任务执行。
 */
class SessionManager {
  public:
    using TaskFactory =
        std::function<zco::Task(const Connection::ptr &, uint64_t)>;

    // 配置和停止状态由包含此管理器的 ServerRun 持有。
    SessionManager(const ServerOptions &options,
                   const std::vector<zco::Executor> &workers,
                   const std::atomic<bool> &running);

    /**
     * @brief 接管连接，登记会话后构建并提交任务。
     *
     * @param make_task
     * 仅构建任务，持会话锁调用；不得在此执行应用回调或重入管理器。
     * @return 是否继续接收；传输流创建失败时报告错误并返回 true，
     * 停止或提交失败时返回 false，因此返回值不代表当前连接已成功建立。
     */
    bool admit(Socket socket, const TaskFactory &make_task);
    void close_connections();
    size_t active_connections() const;

    // 提交的任务持有 ServerRun，使此管理器在任务期间保持存活。
    Result<void> start_reaper(const zco::Executor &executor, zco::Task task);

    /**
     * @brief 登记任务即将结束的通知；回收方仍须等待任务真正完成。
     * @pre id 对应已登记会话，且每个会话只通知一次。
     */
    void session_completed(uint64_t id);
    void finish_accepting();
    void reap_completed();

    /**
     * @brief 等待回收任务结束，再关闭并等待尚未回收的会话。
     * @pre 接收任务已结束，不再调用
     * admit；由控制线程执行，不能等待自身会话任务。
     */
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

    // 通知早于任务最终返回，表项保留到 TaskHandle::join 完成后再移除。
    std::vector<uint64_t> completed_sessions_;

    // 表示不会再登记新连接，供回收任务排完当前通知后退出；由 mutex_ 保护。
    bool accepting_done_ = false;
    uint64_t next_id_ = 0;
};
} // namespace znet::detail

#endif
