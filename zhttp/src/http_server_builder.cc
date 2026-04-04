#include "http_server_builder.h"
#include "daemon.h"
#include "request_body_middleware.h"
#include "zhttp_logger.h"

#include "zcoroutine/log.h"
#include "zcoroutine/sched.h"
#include "znet/address.h"
#include "znet/znet_logger.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <stdexcept>
#include <utility>

namespace zhttp {

namespace {

static zlog::LogLevel::value parse_log_level(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });

  if (s == "debug") {
    return zlog::LogLevel::value::DEBUG;
  }
  if (s == "info") {
    return zlog::LogLevel::value::INFO;
  }
  if (s == "warning" || s == "warn") {
    return zlog::LogLevel::value::WARNING;
  }
  if (s == "error") {
    return zlog::LogLevel::value::ERROR;
  }
  if (s == "fatal") {
    return zlog::LogLevel::value::FATAL;
  }
  return zlog::LogLevel::value::INFO;
}

struct UnifiedLoggerOptions {
  zlog::LogLevel::value level = zlog::LogLevel::value::INFO;
  bool async = true;
  std::string formatter = "[%d{%H:%M:%S}][%c][%p]%T%m%n";
  std::string sink = "stdout";
  std::string file_path;
};

static std::string normalize_token(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

static std::string parse_sink_name(std::string s) {
  s = normalize_token(std::move(s));

  if (s == "file") {
    return "file";
  }
  if (s == "both" || s == "stdout+file" || s == "file+stdout") {
    return "both";
  }
  return "stdout";
}

static UnifiedLoggerOptions apply_module_override(
    const UnifiedLoggerOptions &base, const ModuleLogConfig &override_cfg,
    const std::string &default_file) {
  UnifiedLoggerOptions options = base;

  if (!override_cfg.level.empty()) {
    options.level = parse_log_level(override_cfg.level);
  }
  if (!override_cfg.format.empty()) {
    options.formatter = override_cfg.format;
  }
  if (!override_cfg.sink.empty()) {
    options.sink = parse_sink_name(override_cfg.sink);
  }
  if (!override_cfg.file.empty()) {
    options.file_path = override_cfg.file;
  }
  if (override_cfg.has_async) {
    options.async = override_cfg.async;
  }

  if ((options.sink == "file" || options.sink == "both") &&
      options.file_path.empty()) {
    options.file_path = default_file;
  }

  return options;
}

static zcoroutine::LoggerInitOptions to_coroutine_options(
    const UnifiedLoggerOptions &options) {
  zcoroutine::LoggerInitOptions converted;
  converted.level = options.level;
  converted.async = options.async;
  converted.formatter = options.formatter;
  converted.sink = options.sink;
  converted.file_path = options.file_path;
  return converted;
}

static znet::LoggerInitOptions to_znet_options(
    const UnifiedLoggerOptions &options) {
  znet::LoggerInitOptions converted;
  converted.level = options.level;
  converted.async = options.async;
  converted.formatter = options.formatter;
  converted.sink = options.sink;
  converted.file_path = options.file_path;
  return converted;
}

static zhttp::LoggerInitOptions to_zhttp_options(
    const UnifiedLoggerOptions &options) {
  zhttp::LoggerInitOptions converted;
  converted.level = options.level;
  converted.async = options.async;
  converted.formatter = options.formatter;
  converted.sink = options.sink;
  converted.file_path = options.file_path;
  return converted;
}

static void configure_unified_logging(const ServerConfig &config) {
  UnifiedLoggerOptions global;
  global.level = parse_log_level(config.log_level);
  global.async = config.log_async;
  global.formatter = config.log_format;
  global.sink = parse_sink_name(config.log_sink);
  global.file_path = config.log_file;

  const UnifiedLoggerOptions zcoroutine_options =
      apply_module_override(global, config.zcoroutine_log,
                            "./logfile/zcoroutine.log");
  const UnifiedLoggerOptions znet_options =
      apply_module_override(global, config.znet_log, "./logfile/znet.log");
  const UnifiedLoggerOptions zhttp_options =
      apply_module_override(global, config.zhttp_log, "./logfile/zhttp.log");

  zcoroutine::init_logger(to_coroutine_options(zcoroutine_options));
  znet::init_logger(to_znet_options(znet_options));
  zhttp::init_logger(to_zhttp_options(zhttp_options));
}

static bool is_any_address_host(const std::string &host) {
  return host == "0.0.0.0" || host == "::" || host == "[::]";
}

static std::string strip_host_port(const std::string &host_header) {
  if (host_header.empty()) {
    return "";
  }

  // IPv6: [::1]:8080 -> [::1]
  if (host_header.front() == '[') {
    const std::size_t end = host_header.find(']');
    if (end != std::string::npos) {
      return host_header.substr(0, end + 1);
    }
    return host_header;
  }

  const std::size_t first_colon = host_header.find(':');
  if (first_colon == std::string::npos) {
    return host_header;
  }

  // 没有 [] 包裹但出现多个冒号，通常是 IPv6 字面量，保持原值。
  if (host_header.find(':', first_colon + 1) != std::string::npos) {
    return host_header;
  }

  return host_header.substr(0, first_colon);
}

static std::string make_https_location(const HttpRequest::ptr &request,
                                       const ServerConfig &config) {
  std::string host = strip_host_port(request->header("Host"));
  if (host.empty()) {
    host = config.host;
    if (is_any_address_host(host)) {
      host = "localhost";
    }
  }

  std::string target = "https://" + host;
  if (config.port != 443) {
    target += ":" + std::to_string(config.port);
  }

  const std::string &path = request->path();
  target += path.empty() ? "/" : path;

  const std::string &query = request->query();
  if (!query.empty()) {
    target += "?";
    target += query;
  }

  return target;
}

static void install_force_https_redirect_routes(HttpServer &redirect_server,
                                                const ServerConfig &config) {
  static const std::array<HttpMethod, 9> kMethods = {
      HttpMethod::GET,     HttpMethod::POST,   HttpMethod::PUT,
      HttpMethod::DELETE,  HttpMethod::HEAD,   HttpMethod::OPTIONS,
      HttpMethod::PATCH,   HttpMethod::CONNECT, HttpMethod::TRACE};

  auto redirect_handler =
      [config](const HttpRequest::ptr &req, HttpResponse &resp) {
        resp.redirect(make_https_location(req, config),
                      HttpStatus::PERMANENT_REDIRECT);
      };

  // 同时兜底根路径和任意子路径。
  for (HttpMethod method : kMethods) {
    redirect_server.router().add_route(method, "/", redirect_handler);
    redirect_server.router().add_route(method, "/*path", redirect_handler);
  }
}

} // namespace

HttpServerBuilder::HttpServerBuilder() {
  // 使用默认配置
}


HttpServerBuilder &
HttpServerBuilder::from_config(const std::string &config_path) {
  config_ = ServerConfig::from_toml(config_path);
  return *this;
}

HttpServerBuilder &HttpServerBuilder::read_timeout(uint64_t timeout_ms) {
  config_.read_timeout = timeout_ms;
  return *this;
}

HttpServerBuilder &HttpServerBuilder::write_timeout(uint64_t timeout_ms) {
  config_.write_timeout = timeout_ms;
  return *this;
}

HttpServerBuilder &HttpServerBuilder::keepalive_timeout(uint64_t timeout_ms) {
  config_.keepalive_timeout = timeout_ms;
  return *this;
}


HttpServerBuilder &HttpServerBuilder::listen(const std::string &host,
                                             uint16_t port) {
  config_.host = host;
  config_.port = port;
  return *this;
}

HttpServerBuilder &HttpServerBuilder::threads(size_t num_threads) {
  config_.num_threads = num_threads;
  return *this;
}

HttpServerBuilder &HttpServerBuilder::stack_mode(StackMode mode) {
  config_.stack_mode = mode;
  return *this;
}

HttpServerBuilder &HttpServerBuilder::use_shared_stack() {
  config_.stack_mode = StackMode::SHARED;
  return *this;
}

HttpServerBuilder &HttpServerBuilder::use_independent_stack() {
  config_.stack_mode = StackMode::INDEPENDENT;
  return *this;
}

HttpServerBuilder &
HttpServerBuilder::enable_https(const std::string &cert_file,
                                const std::string &key_file) {
  config_.enable_https = true;
  config_.cert_file = cert_file;
  config_.key_file = key_file;
  return *this;
}

HttpServerBuilder &HttpServerBuilder::force_https_redirect(bool enable,
                                                           uint16_t http_port) {
  config_.force_http_to_https = enable;
  config_.redirect_http_port = http_port;
  return *this;
}

HttpServerBuilder &HttpServerBuilder::use(Middleware::ptr middleware) {
  if (middleware) {
    middlewares_.push_back(std::move(middleware));
  }
  return *this;
}

HttpServerBuilder &HttpServerBuilder::get(const std::string &path,
                                          RouterCallback callback) {
  routes_.emplace_back(HttpMethod::GET, path,
                       RouteHandlerWrapper(std::move(callback)));
  return *this;
}

HttpServerBuilder &HttpServerBuilder::get(const std::string &path,
                                          RouteHandler::ptr handler) {
  routes_.emplace_back(HttpMethod::GET, path,
                       RouteHandlerWrapper(std::move(handler)));
  return *this;
}

HttpServerBuilder &HttpServerBuilder::post(const std::string &path,
                                           RouterCallback callback) {
  routes_.emplace_back(HttpMethod::POST, path,
                       RouteHandlerWrapper(std::move(callback)));
  return *this;
}

HttpServerBuilder &HttpServerBuilder::post(const std::string &path,
                                           RouteHandler::ptr handler) {
  routes_.emplace_back(HttpMethod::POST, path,
                       RouteHandlerWrapper(std::move(handler)));
  return *this;
}

HttpServerBuilder &HttpServerBuilder::put(const std::string &path,
                                          RouterCallback callback) {
  routes_.emplace_back(HttpMethod::PUT, path,
                       RouteHandlerWrapper(std::move(callback)));
  return *this;
}

HttpServerBuilder &HttpServerBuilder::put(const std::string &path,
                                          RouteHandler::ptr handler) {
  routes_.emplace_back(HttpMethod::PUT, path,
                       RouteHandlerWrapper(std::move(handler)));
  return *this;
}

HttpServerBuilder &HttpServerBuilder::del(const std::string &path,
                                          RouterCallback callback) {
  routes_.emplace_back(HttpMethod::DELETE, path,
                       RouteHandlerWrapper(std::move(callback)));
  return *this;
}

HttpServerBuilder &HttpServerBuilder::del(const std::string &path,
                                          RouteHandler::ptr handler) {
  routes_.emplace_back(HttpMethod::DELETE, path,
                       RouteHandlerWrapper(std::move(handler)));
  return *this;
}

HttpServerBuilder &HttpServerBuilder::websocket(
    const std::string &path,
    WebSocketCallbacks callbacks,
    const WebSocketOptions &options) {
  auto callbacks_ref = std::make_shared<WebSocketCallbacks>(std::move(callbacks));

  routes_.emplace_back(
      HttpMethod::GET,
      path,
      RouteHandlerWrapper([callbacks_ref, options](const HttpRequest::ptr &,
                                                   HttpResponse &response) {
        response.upgrade_to_websocket(*callbacks_ref, options);
      }));
  return *this;
}

HttpServerBuilder &HttpServerBuilder::not_found(RouterCallback callback) {
  not_found_handler_ = RouteHandlerWrapper(std::move(callback));
  return *this;
}

HttpServerBuilder &HttpServerBuilder::not_found(RouteHandler::ptr handler) {
  not_found_handler_ = RouteHandlerWrapper(std::move(handler));
  return *this;
}

HttpServerBuilder &
HttpServerBuilder::exception_handler(Router::ExceptionHandler handler) {
  exception_handler_ = std::move(handler);
  return *this;
}

HttpServerBuilder &HttpServerBuilder::log_level(const std::string &level) {
  config_.log_level = level;
  return *this;
}

HttpServerBuilder &HttpServerBuilder::log_async(bool enable) {
  config_.log_async = enable;
  return *this;
}

HttpServerBuilder &HttpServerBuilder::log_format(const std::string &format) {
  config_.log_format = format;
  return *this;
}

HttpServerBuilder &HttpServerBuilder::log_sink(const std::string &sink) {
  config_.log_sink = sink;
  return *this;
}

HttpServerBuilder &HttpServerBuilder::log_file(const std::string &file_path) {
  config_.log_file = file_path;
  return *this;
}

HttpServerBuilder &HttpServerBuilder::daemon(bool enable) {
  config_.daemon = enable;
  return *this;
}

HttpServerBuilder &HttpServerBuilder::homepage(const std::string &path) {
  config_.homepage = path;
  return *this;
}

HttpServerBuilder &HttpServerBuilder::server_name(const std::string &name) {
  config_.server_name = name;
  return *this;
}

std::shared_ptr<HttpServer> HttpServerBuilder::build() {
  redirect_server_.reset();

  // 验证配置
  if (!config_.validate()) {
    throw std::runtime_error("Invalid server configuration");
  }

  configure_unified_logging(config_);

  if (config_.stack_mode == StackMode::SHARED) {
    zcoroutine::co_stack_model(zcoroutine::StackModel::kShared);
  } else {
    zcoroutine::co_stack_model(zcoroutine::StackModel::kIndependent);
  }

  ZHTTP_LOG_INFO("Creating server with {} threads, stack_mode={}",
                 config_.num_threads, stack_mode_to_string(config_.stack_mode));

  auto addrs = znet::Address::lookup(config_.host, config_.port);
  if (addrs.empty()) {
    throw std::runtime_error("Failed to resolve address: " + config_.host +
                             ":" + std::to_string(config_.port));
  }

  // 创建服务器。HTTPS 与 HTTP 统一在 HttpServer 内部处理，避免双分支实现。
  auto server = std::make_shared<HttpServer>(addrs[0]);
  if (config_.enable_https &&
      !server->set_ssl_certificate(config_.cert_file, config_.key_file)) {
    throw std::runtime_error("Failed to initialize SSL certificate");
  }

  server->set_thread_count(config_.num_threads);
  server->set_name(config_.server_name);
  server->set_recv_timeout(config_.read_timeout);
  server->set_write_timeout(config_.write_timeout);
  server->set_keepalive_timeout(config_.keepalive_timeout);

  if (!config_.homepage.empty()) {
    server->router().set_homepage(config_.homepage);
  }

  // 默认启用请求体解析：JSON / x-www-form-urlencoded / multipart。
  server->router().use(std::make_shared<RequestBodyMiddleware>());

  // 注册中间件
  for (auto &mw : middlewares_) {
    server->router().use(mw);
  }

  // 注册路由
  for (auto &route : routes_) {
    HttpMethod method = std::get<0>(route);
    const std::string &path = std::get<1>(route);
    RouteHandlerWrapper &handler = std::get<2>(route);

    // 使用回调包装
    server->router().add_route(
        method, path,
        [handler](const HttpRequest::ptr &req, HttpResponse &resp) {
          handler(req, resp);
        });
  }

  // 设置 404 处理器
  if (not_found_handler_) {
    server->router().set_not_found_handler(
        [handler = not_found_handler_](const HttpRequest::ptr &req,
                                       HttpResponse &resp) {
          handler(req, resp);
        });
  }

  // 设置异常处理器
  if (exception_handler_) {
    server->router().set_exception_handler(exception_handler_);
  }

  if (config_.enable_https && config_.force_http_to_https) {
    auto redirect_addrs =
        znet::Address::lookup(config_.host, config_.redirect_http_port);
    if (redirect_addrs.empty()) {
      throw std::runtime_error("Failed to resolve redirect address: " +
                               config_.host + ":" +
                               std::to_string(config_.redirect_http_port));
    }

    redirect_server_ = std::make_shared<HttpServer>(redirect_addrs[0]);
    redirect_server_->set_thread_count(1);
    redirect_server_->set_name(config_.server_name + " (redirect)");
    redirect_server_->set_recv_timeout(config_.read_timeout);
    redirect_server_->set_write_timeout(config_.write_timeout);
    redirect_server_->set_keepalive_timeout(config_.keepalive_timeout);

    install_force_https_redirect_routes(*redirect_server_, config_);
  }

  return server;
}

void HttpServerBuilder::run() {
  auto run_server = [this](int /*argc*/, char ** /*argv*/) -> int {
    try {
      auto server = build();
      auto redirect_server = redirect_server_;
      ZHTTP_LOG_INFO("Server starting on {}:{}", config_.host, config_.port);

      if (redirect_server) {
        ZHTTP_LOG_INFO("Redirect server starting on {}:{} (http -> https)",
                       config_.host, config_.redirect_http_port);
        if (!redirect_server->start()) {
          ZHTTP_LOG_ERROR("Redirect server failed to start on {}:{}",
                          config_.host, config_.redirect_http_port);
          return -1;
        }
      }

      if (!server->start()) {
        ZHTTP_LOG_ERROR("Server failed to start on {}:{}", config_.host,
                        config_.port);
        if (redirect_server) {
          redirect_server->stop();
        }
        return -1;
      }

      while (!Daemon::should_stop()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }

      ZHTTP_LOG_INFO("Server stopping on {}:{}", config_.host, config_.port);
      server->stop();
      if (redirect_server) {
        ZHTTP_LOG_INFO("Redirect server stopping on {}:{}", config_.host,
                       config_.redirect_http_port);
        redirect_server->stop();
      }
      return 0;
    } catch (const std::exception &ex) {
      ZHTTP_LOG_ERROR("Server run failed: {}", ex.what());
      return -1;
    } catch (...) {
      ZHTTP_LOG_ERROR("Server run failed: unknown exception");
      return -1;
    }
  };

  int rc = Daemon::start_daemon(0, nullptr, std::move(run_server),
                                config_.daemon);
  if (rc != 0) {
    throw std::runtime_error("Server exited with code " +
                             std::to_string(rc));
  }
}

} // namespace zhttp
