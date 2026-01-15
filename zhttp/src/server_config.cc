#include "server_config.h"
#include "zhttp_logger.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <toml.hpp>

namespace zhttp {

StackMode string_to_stack_mode(const std::string &str) {
  if (str == "shared" || str == "SHARED") {
    return StackMode::SHARED;
  }
  return StackMode::INDEPENDENT;
}

std::string stack_mode_to_string(StackMode mode) {
  switch (mode) {
  case StackMode::SHARED:
    return "shared";
  case StackMode::INDEPENDENT:
  default:
    return "independent";
  }
}

ServerConfig ServerConfig::from_toml(const std::string &filepath) {
  ZHTTP_LOG_INFO("Loading config from: {}", filepath);

  try {
    auto data = toml::parse(filepath);
    ServerConfig config;

    // [server] 部分
    if (data.contains("server")) {
      auto &server = toml::find(data, "server");

      if (server.contains("host")) {
        config.host = toml::find<std::string>(server, "host");
      }
      if (server.contains("port")) {
        config.port =
            static_cast<uint16_t>(toml::find<int64_t>(server, "port"));
      }
      if (server.contains("name")) {
        config.server_name = toml::find<std::string>(server, "name");
      }
      if (server.contains("daemon")) {
        config.daemon = toml::find<bool>(server, "daemon");
      }
    }

    // [threads] 部分
    if (data.contains("threads")) {
      auto &threads = toml::find(data, "threads");

      if (threads.contains("count")) {
        config.num_threads =
            static_cast<size_t>(toml::find<int64_t>(threads, "count"));
      }
      if (threads.contains("stack_mode")) {
        std::string mode = toml::find<std::string>(threads, "stack_mode");
        config.stack_mode = string_to_stack_mode(mode);
      }
    }

    // [ssl] 部分
    if (data.contains("ssl")) {
      auto &ssl = toml::find(data, "ssl");

      if (ssl.contains("enabled")) {
        config.enable_https = toml::find<bool>(ssl, "enabled");
      }
      if (ssl.contains("cert_file")) {
        config.cert_file = toml::find<std::string>(ssl, "cert_file");
      }
      if (ssl.contains("key_file")) {
        config.key_file = toml::find<std::string>(ssl, "key_file");
      }
    }

    // [logging] 部分
    if (data.contains("logging")) {
      auto &logging = toml::find(data, "logging");

      if (logging.contains("level")) {
        config.log_level = toml::find<std::string>(logging, "level");
      }
      if (logging.contains("file")) {
        config.log_file = toml::find<std::string>(logging, "file");
      }
    }

    // [timeout] 部分
    if (data.contains("timeout")) {
      auto &timeout = toml::find(data, "timeout");

      if (timeout.contains("read")) {
        config.read_timeout =
            static_cast<uint64_t>(toml::find<int64_t>(timeout, "read"));
      }
      if (timeout.contains("write")) {
        config.write_timeout =
            static_cast<uint64_t>(toml::find<int64_t>(timeout, "write"));
      }
      if (timeout.contains("keepalive")) {
        config.keepalive_timeout =
            static_cast<uint64_t>(toml::find<int64_t>(timeout, "keepalive"));
      }
    }

    // [buffer] 部分
    if (data.contains("buffer")) {
      auto &buffer = toml::find(data, "buffer");

      if (buffer.contains("max_body_size")) {
        config.max_body_size =
            static_cast<size_t>(toml::find<int64_t>(buffer, "max_body_size"));
      }
      if (buffer.contains("size")) {
        config.buffer_size =
            static_cast<size_t>(toml::find<int64_t>(buffer, "size"));
      }
    }

    ZHTTP_LOG_INFO("Config loaded: {}:{}, threads={}, stack_mode={}",
                   config.host, config.port, config.num_threads,
                   stack_mode_to_string(config.stack_mode));

    return config;

  } catch (const toml::syntax_error &e) {
    throw std::runtime_error("TOML syntax error: " + std::string(e.what()));
  } catch (const std::exception &e) {
    throw std::runtime_error("Failed to load config: " + std::string(e.what()));
  }
}

ServerConfig ServerConfig::from_toml_string(const std::string &toml_content) {
  try {
    std::istringstream iss(toml_content);
    auto data = toml::parse(iss);
    ServerConfig config;

    // 复用相同的解析逻辑
    if (data.contains("server")) {
      auto &server = toml::find(data, "server");
      if (server.contains("host")) {
        config.host = toml::find<std::string>(server, "host");
      }
      if (server.contains("port")) {
        config.port =
            static_cast<uint16_t>(toml::find<int64_t>(server, "port"));
      }
      if (server.contains("name")) {
        config.server_name = toml::find<std::string>(server, "name");
      }
      if (server.contains("daemon")) {
        config.daemon = toml::find<bool>(server, "daemon");
      }
    }

    if (data.contains("threads")) {
      auto &threads = toml::find(data, "threads");
      if (threads.contains("count")) {
        config.num_threads =
            static_cast<size_t>(toml::find<int64_t>(threads, "count"));
      }
      if (threads.contains("stack_mode")) {
        std::string mode = toml::find<std::string>(threads, "stack_mode");
        config.stack_mode = string_to_stack_mode(mode);
      }
    }

    if (data.contains("ssl")) {
      auto &ssl = toml::find(data, "ssl");
      if (ssl.contains("enabled")) {
        config.enable_https = toml::find<bool>(ssl, "enabled");
      }
      if (ssl.contains("cert_file")) {
        config.cert_file = toml::find<std::string>(ssl, "cert_file");
      }
      if (ssl.contains("key_file")) {
        config.key_file = toml::find<std::string>(ssl, "key_file");
      }
    }

    if (data.contains("logging")) {
      auto &logging = toml::find(data, "logging");
      if (logging.contains("level")) {
        config.log_level = toml::find<std::string>(logging, "level");
      }
      if (logging.contains("file")) {
        config.log_file = toml::find<std::string>(logging, "file");
      }
    }

    if (data.contains("timeout")) {
      auto &timeout = toml::find(data, "timeout");
      if (timeout.contains("read")) {
        config.read_timeout =
            static_cast<uint64_t>(toml::find<int64_t>(timeout, "read"));
      }
      if (timeout.contains("write")) {
        config.write_timeout =
            static_cast<uint64_t>(toml::find<int64_t>(timeout, "write"));
      }
      if (timeout.contains("keepalive")) {
        config.keepalive_timeout =
            static_cast<uint64_t>(toml::find<int64_t>(timeout, "keepalive"));
      }
    }

    if (data.contains("buffer")) {
      auto &buffer = toml::find(data, "buffer");
      if (buffer.contains("max_body_size")) {
        config.max_body_size =
            static_cast<size_t>(toml::find<int64_t>(buffer, "max_body_size"));
      }
      if (buffer.contains("size")) {
        config.buffer_size =
            static_cast<size_t>(toml::find<int64_t>(buffer, "size"));
      }
    }

    return config;
  } catch (const std::exception &e) {
    throw std::runtime_error("Failed to parse TOML: " + std::string(e.what()));
  }
}

bool ServerConfig::validate() const {
  if (port == 0) {
    ZHTTP_LOG_ERROR("Invalid port: 0");
    return false;
  }

  if (num_threads == 0) {
    ZHTTP_LOG_ERROR("Invalid thread count: 0");
    return false;
  }

  if (enable_https) {
    if (cert_file.empty()) {
      ZHTTP_LOG_ERROR("HTTPS enabled but cert_file is empty");
      return false;
    }
    if (key_file.empty()) {
      ZHTTP_LOG_ERROR("HTTPS enabled but key_file is empty");
      return false;
    }
  }

  return true;
}

std::string ServerConfig::to_toml_string() const {
  std::ostringstream oss;

  oss << "# zhttp server configuration\n\n";

  oss << "[server]\n";
  oss << "host = \"" << host << "\"\n";
  oss << "port = " << port << "\n";
  oss << "name = \"" << server_name << "\"\n";
  oss << "daemon = " << (daemon ? "true" : "false") << "\n\n";

  oss << "[threads]\n";
  oss << "count = " << num_threads << "\n";
  oss << "stack_mode = \"" << stack_mode_to_string(stack_mode) << "\"\n\n";

  oss << "[ssl]\n";
  oss << "enabled = " << (enable_https ? "true" : "false") << "\n";
  if (!cert_file.empty()) {
    oss << "cert_file = \"" << cert_file << "\"\n";
  }
  if (!key_file.empty()) {
    oss << "key_file = \"" << key_file << "\"\n";
  }
  oss << "\n";

  oss << "[logging]\n";
  oss << "level = \"" << log_level << "\"\n";
  if (!log_file.empty()) {
    oss << "file = \"" << log_file << "\"\n";
  }
  oss << "\n";

  oss << "[timeout]\n";
  oss << "read = " << read_timeout << "\n";
  oss << "write = " << write_timeout << "\n";
  oss << "keepalive = " << keepalive_timeout << "\n\n";

  oss << "[buffer]\n";
  oss << "max_body_size = " << max_body_size << "\n";
  oss << "size = " << buffer_size << "\n";

  return oss.str();
}

} // namespace zhttp
