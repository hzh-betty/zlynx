#ifndef ZHTTP_RATE_LIMITER_H_
#define ZHTTP_RATE_LIMITER_H_

#include "http_utils.h"
#include "middleware.h"

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace zhttp {

/**
 * @brief 限流器抽象接口
 *
 * 提供三种标准限流算法：
 * - Fixed Window（固定窗口）
 * - Sliding Window（滑动窗口）
 * - Token Bucket（令牌桶）
 *
 * @note 线程安全：实现类通常需要支持并发调用（同一实例被多个请求共享）。
 *       本库内置实现均在内部加锁保证并发安全，但代价是每次判定需要持锁。
 *
 * @note key 的选择：key 决定限流维度（例如 remote ip / user id / 全局）。
 *       key 的基数越大（例如把完整的 UA、URL 拼进去），内部 map
 * 占用会增长越快。
 */
class RateLimiter {
  public:
    using ptr = std::shared_ptr<RateLimiter>;
    using Clock = TimerHelper::SteadyClock;
    using TimePoint = TimerHelper::SteadyTimePoint;
    using Milliseconds = TimerHelper::Milliseconds;
    using NowFunc = std::function<TimePoint()>;

    enum class Type {
        FIXED_WINDOW, // 按整段时间窗口计数，简单但边界突刺更明显
        SLIDING_WINDOW, // 记录窗口内的请求时间点，限流结果更平滑
        TOKEN_BUCKET, // 以恒定速率补充令牌，允许短时突发
    };

    /**
     * @brief 时间单位枚举
     * @details
     * 不同算法都会把容量和时间单位组合起来解释为“单位时间内允许多少请求”。
     */
    enum class TimeUnit {
        MILLISECOND,
        SECOND,
        MINUTE,
        HOUR,
    };

    virtual ~RateLimiter() = default;

    /**
     * @brief 检查是否允许通过
     *
     * 语义：
     * - 返回 true：本次请求“消耗”一次额度（计数+1 / 消耗 1 令牌等）。
     * - 返回 false：本次请求不应继续处理，通常返回 429。
     *
     * 并发：同一个 key 的判断与扣减应当具有原子性。
     * @param key 维度 key（例如 remote ip / user id / global）
     */
    virtual bool isAllowed(const std::string &key) = 0;

    /**
     * @brief 计算建议的重试等待时间（用于 Retry-After），无法估算返回 0
     *
     * @note
     * 该值是“建议等待时间”，并不保证严格准确（例如实现可能不会在此处推进内部时间状态）。
     *       中间件通常会将其向上取整到秒，写入 Retry-After 响应头。
     */
    virtual Milliseconds retryAfter(const std::string &key) const {
        (void)key;
        return TimerHelper::milliseconds(0);
    }

    /**
     * @brief 工厂方法：创建限流器实例
     *
     * @param type 算法类型
     * @param capacity 窗口容量 / 令牌桶容量
     * @param unit 时间单位（窗口大小为 1 * unit；令牌补充速率为 capacity /
     * unit）
     *             - 对于窗口类算法：窗口大小固定为 1 个单位。
     *             - 对于令牌桶：补充速率为 “capacity 个令牌 / 1 个单位”。
     * @param now_func 自定义时钟（测试可用），为空则使用 steady_clock::now
     */
    static ptr newRateLimiter(Type type, size_t capacity, TimeUnit unit,
                              NowFunc now_func = NowFunc());
};

/**
 * @brief Token Bucket 令牌桶限流器实现
 *
 * 行为要点：
 * - 桶容量为 capacity。
 * - 以恒定速率补充令牌：每经过 1 个时间单位补充 capacity 个令牌。
 * - 每次请求消耗 1 个令牌，不足则拒绝。
 *
 * @note 内部按 key 保存桶状态；该实现不会主动清理长期不活跃 key 的 bucket。
 *       若 key 基数巨大，建议在上层进行归一化（例如按用户 ID，而非按 URL）。
 */
class TokenBucketRateLimiter : public RateLimiter {
  public:
    /**
     * @brief 构造令牌桶限流器
     * @param capacity 桶容量，同时也是每个时间单位内的补充量
     * @param unit 时间单位
     * @param now_func 可选时钟函数，主要用于测试
     */
    TokenBucketRateLimiter(size_t capacity, TimeUnit unit,
                           NowFunc now_func = NowFunc());

    /**
     * @brief 判断当前 key 是否还有可用令牌
     * @param key 限流维度 key
     * @return true 表示放行并消耗 1 个令牌
     */
    bool isAllowed(const std::string &key) override;

    /**
     * @brief 估算下次至少多久后才能再放行
     * @param key 限流维度 key
     * @return 建议等待时间
     */
    Milliseconds retryAfter(const std::string &key) const override;

  private:
    // 统一获取当前时间，便于测试注入自定义时钟。
    TimePoint now() const;
    // 返回一个时间单位对应的秒数，用于计算令牌补充速率。
    std::chrono::duration<double> unitDuration() const;

    size_t capacity_;
    TimeUnit unit_;
    NowFunc now_func_;

    struct Bucket {
        double tokens = 0.0; // 当前可用令牌数，可为小数以保留补充精度
        TimePoint last; // 上次补充令牌的时间点
        bool initialized = false;
    };

    // buckets_ 可能被多个请求并发访问，因此用互斥锁保护。
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Bucket> buckets_;
};

/**
 * @brief Fixed Window 固定窗口限流器实现
 *
 * 行为要点：
 * - 每个 key 维护一个固定长度时间窗口（大小为 1 * unit）。
 * - 在同一窗口内最多允许 capacity 次请求。
 * - 窗口切换时计数清零并重新开始。
 *
 * @note 该算法实现简单、开销低，但在窗口边界可能出现突刺流量。
 */
class FixedWindowRateLimiter final : public RateLimiter {
  public:
    /**
     * @brief 构造固定窗口限流器
     * @param capacity 每个窗口允许的最大请求数
     * @param unit 窗口时间单位（窗口大小固定为 1 个单位）
     * @param now_func 可选时钟函数，主要用于测试
     */
    FixedWindowRateLimiter(size_t capacity, TimeUnit unit,
                           NowFunc now_func = NowFunc());

    /**
     * @brief 判断当前 key 在当前窗口内是否仍可放行
     * @param key 限流维度 key
     * @return true 表示放行并消耗当前窗口内一个配额
     */
    bool isAllowed(const std::string &key) override;

    /**
     * @brief 估算距离当前窗口结束还需等待多久
     * @param key 限流维度 key
     * @return 建议等待时间
     */
    Milliseconds retryAfter(const std::string &key) const override;

  private:
    struct State {
        TimePoint window_start; // 当前固定窗口起点
        size_t count = 0;       // 当前窗口内已通过请求数
        bool initialized = false;
    };

    // states_ 可能被多个请求并发访问，因此用互斥锁保护。
    mutable std::mutex mutex_;
    mutable std::unordered_map<std::string, State> states_;
    size_t capacity_;
    TimeUnit unit_;
    NowFunc now_func_;
};

/**
 * @brief Sliding Window 滑动窗口限流器实现
 *
 * 行为要点：
 * - 每个 key 维护窗口内请求时间点序列。
 * - 每次请求前先清理滑出窗口的旧时间点，再判断当前数量是否小于 capacity。
 * - 相比固定窗口，限流结果更平滑。
 *
 * @note 该算法按请求时间点存储状态，内存占用与窗口内请求数量相关。
 */
class SlidingWindowRateLimiter final : public RateLimiter {
  public:
    /**
     * @brief 构造滑动窗口限流器
     * @param capacity 窗口内允许的最大请求数
     * @param unit 窗口时间单位（窗口大小固定为 1 个单位）
     * @param now_func 可选时钟函数，主要用于测试
     */
    SlidingWindowRateLimiter(size_t capacity, TimeUnit unit,
                             NowFunc now_func = NowFunc());

    /**
     * @brief 判断当前 key 在滑动窗口内是否仍可放行
     * @param key 限流维度 key
     * @return true 表示放行并记录当前请求时间点
     */
    bool isAllowed(const std::string &key) override;

    /**
     * @brief 估算距离释放下一个可用配额还需等待多久
     * @param key 限流维度 key
     * @return 建议等待时间
     */
    Milliseconds retryAfter(const std::string &key) const override;

  private:
    // queues_ 可能被多个请求并发访问，因此用互斥锁保护。
    mutable std::mutex mutex_;
    // 每个 key 对应一个时间点队列，仅保留窗口范围内的请求时间。
    mutable std::unordered_map<std::string, std::deque<TimePoint>> queues_;
    size_t capacity_;
    TimeUnit unit_;
    NowFunc now_func_;
};

/**
 * @brief 限流中间件：基于 RateLimiter 实现
 *
 * 默认行为：
 * - key：优先使用 request->remote_addr()；若为空再尝试
 * X-Forwarded-For；最终退化为 global。
 * - limiter：默认 10 req / second 的令牌桶。
 * - 失败响应：返回 429 + 文本 body，并写入 Retry-After。
 */
class RateLimiterMiddleware : public Middleware {
  public:
    using KeyFunc = std::function<std::string(const HttpRequest::ptr &)>;

    /**
     * @brief 限流中间件配置项
     */
    struct Options {
        Options() : limiter(), key_func(), retry_after_header("Retry-After") {}

        RateLimiter::ptr limiter; // 具体限流器实例；为空时使用默认令牌桶
        KeyFunc key_func; // 从请求中提取限流维度，例如 IP、用户 ID
        std::string retry_after_header; // 被限流时写回的重试头名
    };

    /**
     * @brief 构造限流中间件
     * @param opt 限流器、中间件 key 提取规则等配置
     */
    explicit RateLimiterMiddleware(Options opt = Options());

    /**
     * @brief 在请求进入业务前执行限流判断
     * @param request HTTP 请求对象
     * @param response HTTP 响应对象
     * @return true 表示放行，false 表示已被限流并写好响应
     */
    bool before(const HttpRequest::ptr &request,
                HttpResponse &response) override;

    /**
     * @brief after 钩子，当前实现不做额外处理
     * @param request HTTP 请求对象
     * @param response HTTP 响应对象
     */
    void after(const HttpRequest::ptr &request,
               HttpResponse &response) override;

  private:
    // 默认按客户端来源地址限流；若无法识别则退化为全局限流。
    std::string default_key(const HttpRequest::ptr &request) const;

    Options options_;
};

} // namespace zhttp

#endif // ZHTTP_RATE_LIMITER_H_
