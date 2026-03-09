#include "rate_limiter.h"

#include "http_request.h"

#include <cmath>
#include <cstdint>
#include <deque>

namespace zhttp {

namespace {

// 将配置的时间单位换算成毫秒窗口，供固定窗口和滑动窗口算法使用。
static inline std::chrono::milliseconds unit_to_ms(RateLimiter::TimeUnit unit) {
  switch (unit) {
  case RateLimiter::TimeUnit::MILLISECOND:
    return std::chrono::milliseconds(1);
  case RateLimiter::TimeUnit::SECOND:
    return std::chrono::seconds(1);
  case RateLimiter::TimeUnit::MINUTE:
    return std::chrono::minutes(1);
  case RateLimiter::TimeUnit::HOUR:
    return std::chrono::hours(1);
  }
  return std::chrono::seconds(1);
}

// 测试可传入自定义时间函数；生产环境默认使用 steady_clock。
static inline RateLimiter::NowFunc default_now(RateLimiter::NowFunc f) {
  if (f) {
    return f;
  }
  return [] { return RateLimiter::Clock::now(); };
}

class FixedWindowRateLimiter final : public RateLimiter {
public:
  FixedWindowRateLimiter(size_t capacity, TimeUnit unit, NowFunc now_func)
      : capacity_(capacity), unit_(unit), now_func_(default_now(now_func)) {
    if (capacity_ == 0) {
      capacity_ = 1;
    }
  }

  bool isAllowed(const std::string &key) override {
    // 固定窗口：每个 key 维护一个窗口起点与计数。
    // 优点：实现简单；缺点：窗口边界处可能产生“突刺”（例如两端各打满 capacity）。
    // 复杂度：摊还 O(1)，内存约为 O(key 基数)。
    const auto t = now_func_();
    const auto win = unit_to_ms(unit_);

    std::lock_guard<std::mutex> lock(mutex_);
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

  std::chrono::milliseconds retryAfter(const std::string &key) const override {
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

private:
  struct State {
    TimePoint window_start;
    size_t count = 0;
    bool initialized = false;
  };

  mutable std::mutex mutex_;
  mutable std::unordered_map<std::string, State> states_;
  size_t capacity_;
  TimeUnit unit_;
  NowFunc now_func_;
};

class SlidingWindowRateLimiter final : public RateLimiter {
public:
  SlidingWindowRateLimiter(size_t capacity, TimeUnit unit, NowFunc now_func)
      : capacity_(capacity), unit_(unit), now_func_(default_now(now_func)) {
    if (capacity_ == 0) {
      capacity_ = 1;
    }
  }

  bool isAllowed(const std::string &key) override {
    // 滑动窗口：每个 key 维护一个时间点队列，仅保留最近一个窗口内的请求时间。
    // 优点：限流更平滑；缺点：内存与窗口内请求数相关（最坏 O(capacity) / key）。
    const auto t = now_func_();
    const auto win = unit_to_ms(unit_);

    std::lock_guard<std::mutex> lock(mutex_);
    auto &q = queues_[key];

    // 通过不断弹出队首，移除已滑出窗口的旧请求。
    while (!q.empty()) {
      auto age = std::chrono::duration_cast<std::chrono::milliseconds>(t - q.front());
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

  std::chrono::milliseconds retryAfter(const std::string &key) const override {
    const auto t = now_func_();
    const auto win = unit_to_ms(unit_);

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = queues_.find(key);
    if (it == queues_.end() || it->second.empty()) {
      return std::chrono::milliseconds(0);
    }

    // 最早一条记录过期后，窗口内会释放出一个可用名额。
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t - it->second.front());
    if (elapsed >= win) {
      return std::chrono::milliseconds(0);
    }
    return win - elapsed;
  }

private:
  mutable std::mutex mutex_;
  mutable std::unordered_map<std::string, std::deque<TimePoint>> queues_;
  size_t capacity_;
  TimeUnit unit_;
  NowFunc now_func_;
};

static inline int ceil_div_ms_to_s(std::chrono::milliseconds ms) {
  // HTTP Retry-After 通常使用“秒”为单位的整数；这里做向上取整。
  if (ms.count() <= 0) {
    return 0;
  }
  return static_cast<int>((ms.count() + 999) / 1000);
}

} // namespace

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

RateLimiter::TimePoint TokenBucketRateLimiter::now() const { return now_func_(); }

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

std::chrono::milliseconds TokenBucketRateLimiter::retryAfter(
    const std::string &key) const {
  // 注意：此处为了只读估算，不会推进 b.last / b.tokens（不引入副作用）。
  // 因此在低频访问下，该估算可能偏保守（等待时间略大）。
  const auto t = now();
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

RateLimiterMiddleware::RateLimiterMiddleware(RateLimiterMiddleware::Options opt)
    : options_(std::move(opt)) {
  if (!options_.key_func) {
    options_.key_func =
        [this](const HttpRequest::ptr &req) { return default_key(req); };
  }
  if (!options_.limiter) {
    // 默认按来源维度做每秒 10 次的令牌桶限流。
    options_.limiter =
        RateLimiter::newRateLimiter(RateLimiter::Type::TOKEN_BUCKET, 10,
                                    RateLimiter::TimeUnit::SECOND);
  }
}

std::string RateLimiterMiddleware::default_key(
    const HttpRequest::ptr &request) const {
  // 优先使用服务器侧感知的对端地址，避免完全信任可伪造的转发头。
  // 当服务部署在反向代理之后时，再退回读取 X-Forwarded-For 的首个地址。
  // 若你的部署链路中存在多级代理，建议上层进行可信代理校验与解析。
  if (!request->remote_addr().empty()) {
    return request->remote_addr();
  }

  std::string xff = request->header("X-Forwarded-For");
  if (!xff.empty()) {
    size_t comma = xff.find(',');
    if (comma != std::string::npos) {
      xff = xff.substr(0, comma);
    }
    while (!xff.empty() && (xff.front() == ' ' || xff.front() == '\t')) {
      xff.erase(xff.begin());
    }
    while (!xff.empty() && (xff.back() == ' ' || xff.back() == '\t')) {
      xff.pop_back();
    }
    if (!xff.empty()) {
      return xff;
    }
  }

  return "global";
}

bool RateLimiterMiddleware::before(const HttpRequest::ptr &request,
                                  HttpResponse &response) {
  const std::string key = options_.key_func(request);

  if (options_.limiter->isAllowed(key)) {
    return true;
  }

  // 命中限流时直接终止后续处理，返回 429 和建议重试时间。
  response.status(HttpStatus::TOO_MANY_REQUESTS)
      .content_type("text/plain; charset=utf-8")
      .body("Too Many Requests");

  auto ra = options_.limiter->retryAfter(key);
  int retry_after = ceil_div_ms_to_s(ra);
  if (retry_after <= 0) {
    retry_after = 1;
  }
  // 即便无法准确估算，也尽量给出一个保守的最小值，便于客户端退避。
  response.header(options_.retry_after_header, std::to_string(retry_after));
  return false;
}

void RateLimiterMiddleware::after(const HttpRequest::ptr & /*request*/,
                                 HttpResponse & /*response*/) {}

} // namespace zhttp
