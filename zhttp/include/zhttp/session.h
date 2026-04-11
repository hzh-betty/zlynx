#ifndef ZHTTP_SESSION_H_
#define ZHTTP_SESSION_H_

#include "zhttp/internal/http_utils.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace zhttp {

/**
 * @brief 轻量级会话对象（键值对）
 *
 * 使用场景：
 * - 通过 SessionMiddleware 注入到
 * HttpRequest，上层处理器可读写登录态、临时状态等。
 * - 该对象本身不做持久化，最终是否写回由 SessionMiddleware 的 after() 决定。
 *
 * @note 线程安全：Session 通常是“每个请求一个实例”的使用方式，本身不加锁。
 *       不建议在多个线程间共享同一个 Session 实例。
 */
class Session {
  public:
    using ptr = std::shared_ptr<Session>;
    using Data = std::unordered_map<std::string, std::string>;

    Session(std::string id, bool is_new)
        : id_(std::move(id)), is_new_(is_new) {}

    /**
     * @brief 获取会话 ID
     * @return 当前会话唯一标识
     */
    const std::string &id() const { return id_; }

    /**
     * @brief 判断是否为本次请求中新建的会话
     * @return true 表示该会话此前不存在于存储中
     */
    bool is_new() const { return is_new_; }

    /**
     * @brief 判断会话数据是否被修改过
     * @return true 表示需要在响应阶段写回存储
     */
    bool modified() const { return modified_; }

    /**
     * @brief 读取会话值；不存在时返回默认值
     */
    std::string get(const std::string &key,
                    const std::string &default_val = "") const;

    /**
     * @brief 写入或覆盖会话值，并标记为已修改
     */
    void set(const std::string &key, const std::string &value);

    /**
     * @brief 删除指定键；若键存在则标记为已修改
     */
    void erase(const std::string &key);

    /**
     * @brief 清空全部会话数据
     */
    void clear();

    /**
     * @brief 获取全部会话键值
     * @return 当前数据快照
     */
    const Data &data() const { return data_; }

    /**
     * @brief 整体替换会话数据
     * @param data 新的数据快照
     */
    void set_data(Data data) { data_ = std::move(data); }

    // 在会话成功写回存储或刚从存储恢复后调用，重置脏标记。
    void mark_persisted() {
        is_new_ = false;
        modified_ = false;
    }

  private:
    std::string id_;
    bool is_new_ = false;
    bool modified_ = false;
    Data data_;
};

/**
 * @brief 会话管理器（内存存储）
 *
 * 存储语义：
 * - 仅内存保存（进程重启即丢失），适合轻量服务或开发环境。
 * - 采用滑动过期（sliding expiration）：load()/save() 成功会刷新 expires_at。
 * - save() 覆盖写入：以传入 Session 的 data() 快照替换存储中的 data。
 *
 * 清理策略：
 * - 不启用后台线程；按操作次数触发惰性清理（cleanup_every）。
 * - 清理是 O(store_ 大小) 的线性扫描，因此不宜将 cleanup_every 设得过小。
 *
 * @note session id 的随机性来自 std::mt19937_64（非密码学安全）。
 *       若用于安全敏感场景（例如强认证凭证），建议更换为 CSPRNG。
 */
class SessionManager {
  public:
    using ptr = std::shared_ptr<SessionManager>;

    /**
     * @brief Session 存储配置
     */
    struct Options {
        Options() : ttl(TimerHelper::seconds(1800)), cleanup_every(128) {}
        Options(TimerHelper::Seconds t, size_t cleanupEvery)
            : ttl(t), cleanup_every(cleanupEvery) {}

        TimerHelper::Seconds ttl; // 会话过期时间；每次访问成功后会顺延
        size_t cleanup_every; // 每执行多少次操作触发一次过期清理
    };

    /**
     * @brief 构造内存 Session 管理器
     * @param opt 会话过期时间和清理策略配置
     */
    explicit SessionManager(Options opt = Options());

    /**
     * @brief 按 session id 加载会话
     * @return 会话不存在或已过期时返回 nullptr
     */
    Session::ptr load(const std::string &session_id);

    /**
     * @brief 创建一个新的空会话并立即放入存储
     */
    Session::ptr create();

    /**
     * @brief 持久化当前会话数据，同时刷新过期时间
     */
    void save(const Session &session);

    /**
     * @brief 删除指定会话
     * @param session_id 要删除的会话 ID
     */
    void destroy(const std::string &session_id);

    /**
     * @brief 获取当前管理器配置
     * @return 配置对象引用
     */
    const Options &options() const { return options_; }

  private:
    struct Record {
        Session::Data data;
        TimerHelper::SteadyTimePoint expires_at; // 绝对过期时间点
    };

    // 生成随机 session id，避免可预测性。
    std::string new_session_id();

    // 仅在持锁状态下调用，批量清理已过期的会话记录。
    void cleanup_expired_locked(TimerHelper::SteadyTimePoint now);

    Options options_;
    std::mutex mutex_;
    std::unordered_map<std::string, Record> store_;
    size_t op_count_ = 0;
};

} // namespace zhttp

#endif // ZHTTP_SESSION_H_
