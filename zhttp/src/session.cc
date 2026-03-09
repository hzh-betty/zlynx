#include "session.h"

#include "http_request.h"

#include <random>

namespace zhttp {

std::string Session::get(const std::string &key,
                         const std::string &default_val) const {
  auto it = data_.find(key);
  if (it != data_.end()) {
    return it->second;
  }
  return default_val;
}

void Session::set(const std::string &key, const std::string &value) {
  auto it = data_.find(key);
  if (it != data_.end() && it->second == value) {
    return;
  }
  data_[key] = value;
  modified_ = true;
}

void Session::erase(const std::string &key) {
  auto it = data_.find(key);
  if (it == data_.end()) {
    return;
  }
  data_.erase(it);
  modified_ = true;
}

void Session::clear() {
  if (data_.empty()) {
    return;
  }
  data_.clear();
  modified_ = true;
}

SessionManager::SessionManager(SessionManager::Options opt)
    : options_(std::move(opt)) {}

Session::ptr SessionManager::load(const std::string &session_id) {
  if (session_id.empty()) {
    return nullptr;
  }

  auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(mutex_);

  ++op_count_;
  if (options_.cleanup_every > 0 && (op_count_ % options_.cleanup_every) == 0) {
    // 采用惰性清理，避免每次请求都线性扫描整个存储。
    cleanup_expired_locked(now);
  }

  auto it = store_.find(session_id);
  if (it == store_.end()) {
    return nullptr;
  }

  if (it->second.expires_at <= now) {
    store_.erase(it);
    return nullptr;
  }

  // 滑动过期：只要会话持续被访问，就顺延有效期。
  it->second.expires_at = now + options_.ttl;

  // 从存储快照构造一个 Session 实例（与存储层数据解耦，避免外部直接改 store_）。
  auto s = std::make_shared<Session>(session_id, false);
  s->set_data(it->second.data);
  s->mark_persisted();
  return s;
}

Session::ptr SessionManager::create() {
  auto now = std::chrono::steady_clock::now();
  std::string id = new_session_id();

  // 先创建逻辑会话对象，再把空记录放入存储中等待后续写入。
  auto s = std::make_shared<Session>(id, true);

  std::lock_guard<std::mutex> lock(mutex_);
  ++op_count_;
  if (options_.cleanup_every > 0 && (op_count_ % options_.cleanup_every) == 0) {
    cleanup_expired_locked(now);
  }

  // 先放入一个空记录，确保同一 id 可以被后续请求 load() 到。
  store_[id] = Record{Session::Data{}, now + options_.ttl};
  return s;
}

void SessionManager::save(const Session &session) {
  auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(mutex_);

  ++op_count_;
  if (options_.cleanup_every > 0 && (op_count_ % options_.cleanup_every) == 0) {
    cleanup_expired_locked(now);
  }

  // 保存时整体覆盖当前快照，并刷新 TTL。
  // 注意：不会做字段级 merge；因此同一个 session 的并发写入可能“后写覆盖先写”。
  store_[session.id()] = Record{session.data(), now + options_.ttl};
}

void SessionManager::destroy(const std::string &session_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  store_.erase(session_id);
}

std::string SessionManager::new_session_id() {
  // 128-bit 随机 ID（32 个十六进制字符），足够用于内存会话场景。
  // 这里不引入锁：thread_local RNG 可避免多线程竞争。
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  auto rand64 = [&]() -> uint64_t { return rng(); };

  uint64_t a = rand64();
  uint64_t b = rand64();

  auto hex = [](uint64_t x) {
    static const char *k = "0123456789abcdef";
    std::string s;
    s.resize(16);
    for (int i = 15; i >= 0; --i) {
      s[static_cast<size_t>(i)] = k[x & 0xF];
      x >>= 4;
    }
    return s;
  };

  return hex(a) + hex(b);
}

void SessionManager::cleanup_expired_locked(
    std::chrono::steady_clock::time_point now) {
  // 原地擦除已过期记录，避免额外拷贝。
  for (auto it = store_.begin(); it != store_.end();) {
    if (it->second.expires_at <= now) {
      it = store_.erase(it);
    } else {
      ++it;
    }
  }
}

SessionMiddleware::SessionMiddleware(SessionManager::ptr manager,
                                     SessionMiddleware::Options opt)
    : manager_(std::move(manager)), options_(std::move(opt)) {}

bool SessionMiddleware::before(const HttpRequest::ptr &request,
                               HttpResponse & /*response*/) {
  if (!manager_) {
    return true;
  }

  // 优先尝试复用客户端已携带的 session id。
  std::string sid = request->cookie(options_.cookie_name);
  Session::ptr s;
  if (!sid.empty()) {
    s = manager_->load(sid);
  }

  // 可选地为匿名新请求创建会话，便于处理登录态、购物车等场景。
  if (!s && options_.create_if_missing) {
    s = manager_->create();
  }

  if (s) {
    // 将会话对象挂到请求上，供后续中间件和业务处理器共享。
    request->set_session(s);
  }

  return true;
}

void SessionMiddleware::after(const HttpRequest::ptr &request,
                              HttpResponse &response) {
  if (!manager_) {
    return;
  }

  auto s = request->session();
  if (!s) {
    return;
  }

  if (s->is_new() || s->modified()) {
    // 保存发生在 set_cookie 之前：确保客户端拿到的 id 在服务端已经可用。
    manager_->save(*s);

    HttpResponse::CookieOptions cookie_opt = options_.cookie;
    cookie_opt.max_age =
        static_cast<int>(manager_->options().ttl.count());

    // 仅在新建或发生修改时回写 Cookie，减少无意义的响应头噪声。
    response.set_cookie(options_.cookie_name, s->id(), cookie_opt);
    s->mark_persisted();
  }
}

} // namespace zhttp
