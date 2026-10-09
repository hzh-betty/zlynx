#include "zhttp/rate_limiter.h"
#include <cmath>
namespace zhttp::mid {
namespace {
// 将配置的时间单位换算成毫秒窗口，供固定窗口和滑动窗口算法使用。
static inline RateLimiter::Milliseconds unit_to_ms(RateLimiter::TimeUnit unit) {
    switch (unit) {
    case RateLimiter::TimeUnit::MILLISECOND:
        return std::chrono::milliseconds(1);
    case RateLimiter::TimeUnit::SECOND:
        return std::chrono::seconds(1);
    case RateLimiter::TimeUnit::MINUTE:
        return std::chrono::seconds(60);
    case RateLimiter::TimeUnit::HOUR:
        return std::chrono::seconds(3600);
    }
    return std::chrono::seconds(1);
}

// 测试可传入自定义时间函数；生产环境默认使用 steady_clock。
static inline RateLimiter::NowFunc default_now(RateLimiter::NowFunc f) {
    if (f) {
        return f;
    }
    return [] { return std::chrono::steady_clock::now(); };
}

}
FixedWindowRateLimiter::FixedWindowRateLimiter(size_t capacity, TimeUnit unit,
                                               NowFunc now_func)
    : capacity_(capacity), unit_(unit), now_func_(default_now(now_func)) {
    if (capacity_ == 0) {
        capacity_ = 1;
    }
}

bool FixedWindowRateLimiter::isAllowed(const std::string &key) {
    // 固定窗口：每个 key 维护一个窗口起点与计数。
    // 优点：实现简单；缺点：窗口边界处可能产生“突刺”（例如两端各打满
    // capacity）。 复杂度：摊还 O(1)，内存约为 O(key 基数)。
    const auto t = now_func_();
    const auto win = unit_to_ms(unit_);

    std::lock_guard<std::mutex> lock(mutex_);
    if (t >= next_cleanup_) {
        prune_locked(t);
        next_cleanup_ = t + win;
    }
    auto &s = states_[key];

    if (!s.initialized) {
        s.initialized = true;
        s.window_start = t;
        s.count = 0;
    }

    // 固定窗口按整块时间段计数，窗口一过立即清零重新开始。
    if (t - s.window_start >= win) {
        s.window_start = t;
        s.count = 0;
    }

    if (s.count < capacity_) {
        ++s.count;
        return true;
    }
    return false;
}

RateLimiter::Milliseconds
FixedWindowRateLimiter::retryAfter(const std::string &key) const {
    const auto t = now_func_();
    const auto win = unit_to_ms(unit_);

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = states_.find(key);
    if (it == states_.end() || !it->second.initialized) {
        return std::chrono::milliseconds(0);
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        t - it->second.window_start);
    if (elapsed >= win) {
        return std::chrono::milliseconds(0);
    }
    return win - elapsed;
}

SlidingWindowRateLimiter::SlidingWindowRateLimiter(size_t capacity,
                                                   TimeUnit unit,
                                                   NowFunc now_func)
    : capacity_(capacity), unit_(unit), now_func_(default_now(now_func)) {
    if (capacity_ == 0) {
        capacity_ = 1;
    }
}

bool SlidingWindowRateLimiter::isAllowed(const std::string &key) {
    // 滑动窗口：每个 key 维护一个时间点队列，仅保留最近一个窗口内的请求时间。
    // 优点：限流更平滑；缺点：内存与窗口内请求数相关（最坏 O(capacity) /
    // key）。
    const auto t = now_func_();
    const auto win = unit_to_ms(unit_);

    std::lock_guard<std::mutex> lock(mutex_);
    if (t >= next_cleanup_) {
        prune_locked(t);
        next_cleanup_ = t + win;
    }
    auto &q = queues_[key];

    // 通过不断弹出队首，移除已滑出窗口的旧请求。
    while (!q.empty()) {
        auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
            t - q.front());
        if (age >= win) {
            q.pop_front();
        } else {
            break;
        }
    }

    if (q.size() < capacity_) {
        q.push_back(t);
        return true;
    }

    return false;
}

RateLimiter::Milliseconds
SlidingWindowRateLimiter::retryAfter(const std::string &key) const {
    const auto t = now_func_();
    const auto win = unit_to_ms(unit_);

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = queues_.find(key);
    if (it == queues_.end() || it->second.empty()) {
        return std::chrono::milliseconds(0);
    }

    // 最早一条记录过期后，窗口内会释放出一个可用名额。
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        t - it->second.front());
    if (elapsed >= win) {
        return std::chrono::milliseconds(0);
    }
    return win - elapsed;
}

RateLimiter::ptr RateLimiter::newRateLimiter(Type type, size_t capacity,
                                             TimeUnit unit, NowFunc now_func) {
    switch (type) {
    case Type::FIXED_WINDOW:
        return std::make_shared<FixedWindowRateLimiter>(capacity, unit,
                                                        std::move(now_func));
    case Type::SLIDING_WINDOW:
        return std::make_shared<SlidingWindowRateLimiter>(capacity, unit,
                                                          std::move(now_func));
    case Type::TOKEN_BUCKET:
    default:
        return std::make_shared<TokenBucketRateLimiter>(capacity, unit,
                                                        std::move(now_func));
    }
}

TokenBucketRateLimiter::TokenBucketRateLimiter(size_t capacity, TimeUnit unit,
                                               NowFunc now_func)
    : capacity_(capacity), unit_(unit), now_func_(default_now(now_func)) {
    if (capacity_ == 0) {
        capacity_ = 1;
    }
}

RateLimiter::TimePoint TokenBucketRateLimiter::now() const {
    return now_func_();
}

std::chrono::duration<double> TokenBucketRateLimiter::unitDuration() const {
    auto ms = unit_to_ms(unit_);
    return std::chrono::duration<double>(ms.count() / 1000.0);
}

bool TokenBucketRateLimiter::isAllowed(const std::string &key) {
    // 令牌桶：按时间流逝补充令牌（可累计小数），每次请求扣减 1。
    // 复杂度：摊还 O(1)，内存约为 O(key 基数)。
    const auto t = now();
    const double cap = static_cast<double>(capacity_);
    const double rate = cap / unitDuration().count(); // 每秒补充的令牌数

    bool allowed = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (t >= next_cleanup_) {
            prune_locked(t);
            next_cleanup_ = t + unit_to_ms(unit_);
        }
        Bucket &b = buckets_[key];

        if (!b.initialized) {
            // 首次访问时桶是满的，允许容量范围内的瞬时突发。
            b.initialized = true;
            b.tokens = cap;
            b.last = t;
        }

        std::chrono::duration<double> elapsed = t - b.last;
        b.last = t;

        // 按时间流逝补充令牌，但不会超过桶容量上限。
        b.tokens += elapsed.count() * rate;
        if (b.tokens > cap) {
            b.tokens = cap;
        }

        if (b.tokens >= 1.0) {
            // 每次请求消耗一个令牌；不足一个令牌则拒绝。
            b.tokens -= 1.0;
            allowed = true;
        }
    }

    return allowed;
}

RateLimiter::Milliseconds
TokenBucketRateLimiter::retryAfter(const std::string &key) const {
    // 注意：此处为了只读估算，不会推进 b.last / b.tokens（不引入副作用）。
    // 因此在低频访问下，该估算可能偏保守（等待时间略大）。
    const double cap = static_cast<double>(capacity_);
    const double rate = cap / unitDuration().count();

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = buckets_.find(key);
    if (it == buckets_.end() || !it->second.initialized) {
        return std::chrono::milliseconds(0);
    }

    const Bucket &b = it->second;
    // 以当前剩余令牌推算距离下一个完整令牌还需等待多久。
    if (b.tokens >= 1.0) {
        return std::chrono::milliseconds(0);
    }

    double deficit = 1.0 - b.tokens;
    double seconds = deficit / rate;
    if (seconds < 0) {
        seconds = 0;
    }

    auto ms = static_cast<int64_t>(std::ceil(seconds * 1000.0));
    if (ms < 0) {
        ms = 0;
    }
    return std::chrono::milliseconds(ms);
}


size_t FixedWindowRateLimiter::prune_locked(TimePoint time) {
    size_t removed = 0;
    for (auto it = states_.begin(); it != states_.end();) {
        if (time - it->second.window_start >= unit_to_ms(unit_)) {
            it = states_.erase(it);
            ++removed;
        } else ++it;
    }
    return removed;
}
size_t SlidingWindowRateLimiter::prune_locked(TimePoint time) {
    size_t removed = 0;
    for (auto it = queues_.begin(); it != queues_.end();) {
        if (it->second.empty() || time - it->second.back() >= unit_to_ms(unit_)) {
            it = queues_.erase(it);
            ++removed;
        } else ++it;
    }
    return removed;
}
size_t TokenBucketRateLimiter::prune_locked(TimePoint time) {
    size_t removed = 0;
    for (auto it = buckets_.begin(); it != buckets_.end();) {
        // 一个完整补充周期后必然已恢复为满桶，删掉等同下一次创建满桶。
        if (time - it->second.last >= unit_to_ms(unit_)) {
            it = buckets_.erase(it);
            ++removed;
        } else ++it;
    }
    return removed;
}
size_t FixedWindowRateLimiter::prune_expired() {
    const auto time = now_func_();
    std::lock_guard<std::mutex> lock(mutex_);
    return prune_locked(time);
}
size_t SlidingWindowRateLimiter::prune_expired() {
    const auto time = now_func_();
    std::lock_guard<std::mutex> lock(mutex_);
    return prune_locked(time);
}
size_t TokenBucketRateLimiter::prune_expired() {
    const auto time = now();
    std::lock_guard<std::mutex> lock(mutex_);
    return prune_locked(time);
}
} // namespace zhttp::mid
