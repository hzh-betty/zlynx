#include "http_server_builder.h"
#include "address.h"
#include "daemon.h"
#include "https_server.h"
#include "zhttp_logger.h"

namespace zhttp {

HttpServerBuilder::HttpServerBuilder() {
  // 使用默认配置
}

// ========== 从配置初始化 ==========

HttpServerBuilder &
HttpServerBuilder::from_config(const std::string &config_path) {
  config_ = ServerConfig::from_toml(config_path);
  return *this;
}

HttpServerBuilder &HttpServerBuilder::from_config(const ServerConfig &config) {
  config_ = config;
  return *this;
}

// ========== 链式配置 API ==========

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

HttpServerBuilder &HttpServerBuilder::not_found(RouterCallback callback) {
  not_found_handler_ = RouteHandlerWrapper(std::move(callback));
  return *this;
}

HttpServerBuilder &HttpServerBuilder::not_found(RouteHandler::ptr handler) {
  not_found_handler_ = RouteHandlerWrapper(std::move(handler));
  return *this;
}

HttpServerBuilder &HttpServerBuilder::log_level(const std::string &level) {
  config_.log_level = level;
  return *this;
}

HttpServerBuilder &HttpServerBuilder::daemon(bool enable) {
  config_.daemon = enable;
  return *this;
}

HttpServerBuilder &HttpServerBuilder::server_name(const std::string &name) {
  config_.server_name = name;
  return *this;
}

std::shared_ptr<HttpServer> HttpServerBuilder::build() {
  // 验证配置
  if (!config_.validate()) {
    throw std::runtime_error("Invalid server configuration");
  }

  // 设置日志级别
  if (config_.log_level == "debug") {
    init_logger(zlog::LogLevel::value::DEBUG);
  } else if (config_.log_level == "info") {
    init_logger(zlog::LogLevel::value::INFO);
  } else if (config_.log_level == "warning" || config_.log_level == "warn") {
    init_logger(zlog::LogLevel::value::WARNING);
  } else if (config_.log_level == "error") {
    init_logger(zlog::LogLevel::value::ERROR);
  }

  // 守护进程模式
  if (config_.daemon) {
    Daemon::daemonize();
  }

  // 根据栈模式创建 IoScheduler
  bool use_shared = (config_.stack_mode == StackMode::SHARED);
  io_scheduler_ = std::make_shared<zcoroutine::IoScheduler>(
      static_cast<int>(config_.num_threads), "zhttp-io", use_shared);

  ZHTTP_LOG_INFO("Creating server with {} threads, stack_mode={}",
                 config_.num_threads, stack_mode_to_string(config_.stack_mode));

  // 创建服务器
  std::shared_ptr<HttpServer> server;

  if (config_.enable_https) {
    auto https_server = std::make_shared<HttpsServer>(io_scheduler_, nullptr);
    https_server->set_ssl_certificate(config_.cert_file, config_.key_file);
    server = https_server;
  } else {
    server = std::make_shared<HttpServer>(io_scheduler_, nullptr);
  }

  server->set_name(config_.server_name);

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

  // 绑定地址并开始监听
  auto addrs =
      znet::Address::lookup(config_.host + ":" + std::to_string(config_.port));
  if (addrs.empty()) {
    throw std::runtime_error("Failed to resolve address: " + config_.host);
  }
  server->bind(addrs[0]);

  return server;
}

void HttpServerBuilder::run() {
  auto server = build();

  ZHTTP_LOG_INFO("Server starting on {}:{}", config_.host, config_.port);

  // 启动 IO 调度器
  io_scheduler_->start();

  // 开始接受连接
  server->start();

  // 等待停止信号
  // 这里简化处理，实际应该有信号处理
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

} // namespace zhttp
