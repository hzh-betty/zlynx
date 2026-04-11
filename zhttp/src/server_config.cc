#include "zhttp/server_config.h"
#include "zhttp/zhttp_logger.h"

#include <stdexcept>

#include <toml.hpp>

namespace zhttp {

namespace {

uint16_t parse_port(const toml::value &table, const char *key) {
    const auto value = toml::find<int64_t>(table, key);
    if (value < 0 || value > 65535) {
        throw std::runtime_error(std::string(key) +
                                 " must be in range [0, 65535]");
    }
    return static_cast<uint16_t>(value);
}

size_t parse_size(const toml::value &table, const char *key) {
    const auto value = toml::find<int64_t>(table, key);
    if (value < 0) {
        throw std::runtime_error(std::string(key) + " must be >= 0");
    }
    return static_cast<size_t>(value);
}

uint64_t parse_timeout_ms(const toml::value &table, const char *key) {
    const auto value = toml::find<int64_t>(table, key);
    if (value < 0) {
        throw std::runtime_error(std::string(key) + " must be >= 0");
    }
    return static_cast<uint64_t>(value);
}

void parse_server_section(const toml::value &data, ServerConfig &config) {
    if (!data.contains("server")) {
        return;
    }

    auto &server = toml::find(data, "server");
    if (server.contains("host")) {
        config.host = toml::find<std::string>(server, "host");
    }
    if (server.contains("port")) {
        config.port = parse_port(server, "port");
    }
    if (server.contains("name")) {
        config.server_name = toml::find<std::string>(server, "name");
    }
    if (server.contains("homepage")) {
        config.homepage = toml::find<std::string>(server, "homepage");
    }
    if (server.contains("daemon")) {
        config.daemon = toml::find<bool>(server, "daemon");
    }
}

void parse_threads_section(const toml::value &data, ServerConfig &config) {
    if (!data.contains("threads")) {
        return;
    }

    auto &threads = toml::find(data, "threads");
    if (threads.contains("count")) {
        config.num_threads = parse_size(threads, "count");
    }
    if (threads.contains("stack_mode")) {
        std::string mode = toml::find<std::string>(threads, "stack_mode");
        config.stack_mode = string_to_stack_mode(mode);
    }
}

void parse_ssl_section(const toml::value &data, ServerConfig &config) {
    if (!data.contains("ssl")) {
        return;
    }

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
    if (ssl.contains("force_http_to_https")) {
        config.force_http_to_https =
            toml::find<bool>(ssl, "force_http_to_https");
    }
    if (ssl.contains("force_redirect")) {
        config.force_http_to_https = toml::find<bool>(ssl, "force_redirect");
    }
    if (ssl.contains("redirect_http_port")) {
        config.redirect_http_port = parse_port(ssl, "redirect_http_port");
    }
}

void parse_logging_section(const toml::value &data, ServerConfig &config) {
    if (!data.contains("logging")) {
        return;
    }

    auto &logging = toml::find(data, "logging");
    if (logging.contains("level")) {
        config.log_level = toml::find<std::string>(logging, "level");
    }
    if (logging.contains("async")) {
        config.log_async = toml::find<bool>(logging, "async");
    }
    if (logging.contains("format")) {
        config.log_format = toml::find<std::string>(logging, "format");
    }
    if (logging.contains("sink")) {
        config.log_sink = toml::find<std::string>(logging, "sink");
    }
    if (logging.contains("file")) {
        config.log_file = toml::find<std::string>(logging, "file");
    }

    if (!logging.contains("modules")) {
        return;
    }

    auto parse_module = [&logging](const char *module_name,
                                   ModuleLogConfig &module_config) {
        const auto &modules = toml::find(logging, "modules");
        if (!modules.contains(module_name)) {
            return;
        }

        const auto &module = toml::find(modules, module_name);
        if (module.contains("level")) {
            module_config.level = toml::find<std::string>(module, "level");
        }
        if (module.contains("async")) {
            module_config.async = toml::find<bool>(module, "async");
            module_config.has_async = true;
        }
        if (module.contains("format")) {
            module_config.format = toml::find<std::string>(module, "format");
        }
        if (module.contains("sink")) {
            module_config.sink = toml::find<std::string>(module, "sink");
        }
        if (module.contains("file")) {
            module_config.file = toml::find<std::string>(module, "file");
        }
    };

    parse_module("zco", config.zco_log);
    parse_module("znet", config.znet_log);
    parse_module("zhttp", config.zhttp_log);
}

void parse_timeout_section(const toml::value &data, ServerConfig &config) {
    if (!data.contains("timeout")) {
        return;
    }

    auto &timeout = toml::find(data, "timeout");
    if (timeout.contains("read")) {
        config.read_timeout = parse_timeout_ms(timeout, "read");
    }
    if (timeout.contains("write")) {
        config.write_timeout = parse_timeout_ms(timeout, "write");
    }
    if (timeout.contains("keepalive")) {
        config.keepalive_timeout = parse_timeout_ms(timeout, "keepalive");
    }
}

void reject_unsupported_sections(const toml::value &data) {
    if (data.contains("buffer")) {
        throw std::runtime_error(
            "[buffer] section is no longer supported; remove it from the TOML "
            "configuration");
    }
}

ServerConfig parse_server_config(const toml::value &data) {
    ServerConfig config;
    parse_server_section(data, config);
    parse_threads_section(data, config);
    parse_ssl_section(data, config);
    parse_logging_section(data, config);
    parse_timeout_section(data, config);
    reject_unsupported_sections(data);
    return config;
}

} // namespace

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
        ServerConfig config = parse_server_config(data);

        ZHTTP_LOG_INFO("Config loaded: {}:{}, threads={}, stack_mode={}",
                       config.host, config.port, config.num_threads,
                       stack_mode_to_string(config.stack_mode));

        return config;

    } catch (const toml::syntax_error &e) {
        throw std::runtime_error("TOML syntax error: " + std::string(e.what()));
    } catch (const std::exception &e) {
        throw std::runtime_error("Failed to load config: " +
                                 std::string(e.what()));
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

    if (force_http_to_https && !enable_https) {
        ZHTTP_LOG_ERROR("force_http_to_https requires HTTPS enabled");
        return false;
    }

    if (force_http_to_https) {
        if (redirect_http_port == 0) {
            ZHTTP_LOG_ERROR("redirect_http_port must not be 0 when "
                            "force_http_to_https is enabled");
            return false;
        }
        if (redirect_http_port == port) {
            ZHTTP_LOG_ERROR(
                "redirect_http_port ({}) must be different from HTTPS "
                "listen port ({})",
                redirect_http_port, port);
            return false;
        }
    }

    if (homepage == "/" || homepage == "/home" || homepage == "home") {
        ZHTTP_LOG_ERROR("Invalid homepage: {} would cause redirect loop",
                        homepage);
        return false;
    }

    return true;
}
} // namespace zhttp
