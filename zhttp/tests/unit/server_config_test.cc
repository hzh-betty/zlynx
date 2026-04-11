#include "zhttp/http_server_builder.h"
#include "zhttp/server_config.h"
#include "zhttp/zhttp_logger.h"
#include "zco/zco_log.h"
#include "znet/znet_logger.h"

#include <cstdio>
#include <fstream>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {

class TempTomlFile {
  public:
    explicit TempTomlFile(const std::string &content) {
        char path[] = "/tmp/zhttp-server-config-XXXXXX.toml";
        const int fd = mkstemps(path, 5);
        if (fd < 0) {
            throw std::runtime_error("failed to create temp file");
        }

        path_ = path;
        std::ofstream output(path_);
        output << content;
        output.close();
        close(fd);
    }

    ~TempTomlFile() {
        if (!path_.empty()) {
            std::remove(path_.c_str());
        }
    }

    const std::string &path() const { return path_; }

  private:
    std::string path_;
};

} // namespace

TEST(ServerConfigTest, LoadsTimeoutsFromTomlFile) {
    TempTomlFile config_file(R"(
[server]
host = "127.0.0.1"
port = 18080

[threads]
count = 2

[timeout]
read = 123
write = 456
keepalive = 789
)");

    const zhttp::ServerConfig config =
        zhttp::ServerConfig::from_toml(config_file.path());

    EXPECT_EQ(config.host, "127.0.0.1");
    EXPECT_EQ(config.port, 18080);
    EXPECT_EQ(config.num_threads, 2U);
    EXPECT_EQ(config.read_timeout, 123U);
    EXPECT_EQ(config.write_timeout, 456U);
    EXPECT_EQ(config.keepalive_timeout, 789U);
}

TEST(ServerConfigTest, IgnoresRateLimitSectionFromTomlFile) {
    TempTomlFile config_file(R"(
[server]
host = "127.0.0.1"
port = 18083

[threads]
count = 1

[rate_limit]
enabled = true
type = "fixed_window"
capacity = 1
time_unit = "minute"
)");

    zhttp::HttpServerBuilder builder;
    builder.from_config(config_file.path());
    builder.get("/ping",
                [](const zhttp::HttpRequest::ptr &, zhttp::HttpResponse &resp) {
                    resp.status(zhttp::HttpStatus::OK).text("pong");
                });

    auto server = builder.build();
    ASSERT_TRUE(server);

    auto req1 = std::make_shared<zhttp::HttpRequest>();
    req1->set_method(zhttp::HttpMethod::GET);
    req1->set_path("/ping");
    zhttp::HttpResponse resp1;
    server->router().route(req1, resp1);
    EXPECT_EQ(resp1.status_code(), zhttp::HttpStatus::OK);

    auto req2 = std::make_shared<zhttp::HttpRequest>();
    req2->set_method(zhttp::HttpMethod::GET);
    req2->set_path("/ping");
    zhttp::HttpResponse resp2;
    server->router().route(req2, resp2);
    EXPECT_EQ(resp2.status_code(), zhttp::HttpStatus::OK);
}

TEST(ServerConfigTest, RejectsRemovedBufferSection) {
    TempTomlFile config_file(R"(
[server]
port = 8080

[buffer]
size = 8192
)");

    EXPECT_THROW(
        {
            try {
                (void)zhttp::ServerConfig::from_toml(config_file.path());
            } catch (const std::runtime_error &error) {
                EXPECT_NE(std::string(error.what()).find("[buffer]"),
                          std::string::npos);
                throw;
            }
        },
        std::runtime_error);
}

TEST(ServerConfigTest, RejectsNegativeTimeoutValues) {
    TempTomlFile config_file(R"(
[server]
port = 8080

[timeout]
read = -1
)");

    EXPECT_THROW((void)zhttp::ServerConfig::from_toml(config_file.path()),
                 std::runtime_error);
}

TEST(ServerConfigTest, BuilderSupportsTimeoutChaining) {
    zhttp::HttpServerBuilder builder;

    builder.read_timeout(100).write_timeout(200).keepalive_timeout(300);

    EXPECT_EQ(builder.config().read_timeout, 100U);
    EXPECT_EQ(builder.config().write_timeout, 200U);
    EXPECT_EQ(builder.config().keepalive_timeout, 300U);
}

TEST(ServerConfigTest, BuilderSupportsExceptionHandlerChaining) {
    zhttp::HttpServerBuilder builder;

    auto &ref = builder.exception_handler([](const zhttp::HttpRequest::ptr &,
                                             zhttp::HttpResponse &resp,
                                             std::exception_ptr) {
        resp.status(zhttp::HttpStatus::INTERNAL_SERVER_ERROR)
            .text("builder-custom");
    });

    EXPECT_EQ(&ref, &builder);
}

TEST(ServerConfigTest, LoadsHttpsRedirectOptionsFromTomlFile) {
    TempTomlFile config_file(R"(
[server]
host = "127.0.0.1"
port = 18443

[threads]
count = 1

[ssl]
enabled = true
cert_file = "/tmp/server.crt"
key_file = "/tmp/server.key"
force_http_to_https = true
redirect_http_port = 18080
)");

    const zhttp::ServerConfig config =
        zhttp::ServerConfig::from_toml(config_file.path());

    EXPECT_TRUE(config.enable_https);
    EXPECT_TRUE(config.force_http_to_https);
    EXPECT_EQ(config.redirect_http_port, 18080);
}

TEST(ServerConfigTest, RejectsHttpsRedirectWhenHttpsIsDisabled) {
    zhttp::ServerConfig config;
    config.enable_https = false;
    config.force_http_to_https = true;
    config.redirect_http_port = 8080;

    EXPECT_FALSE(config.validate());
}

TEST(ServerConfigTest, BuilderSupportsHttpsRedirectChaining) {
    zhttp::HttpServerBuilder builder;

    builder.force_https_redirect(true, 18080);

    EXPECT_TRUE(builder.config().force_http_to_https);
    EXPECT_EQ(builder.config().redirect_http_port, 18080);
}

TEST(ServerConfigTest, LoadsLoggingOverridesFromTomlFile) {
    TempTomlFile config_file(R"(
[server]
host = "127.0.0.1"
port = 18081

[threads]
count = 1

[logging]
level = "warning"
async = false
format = "[%p] %m%n"
sink = "both"
file = "/tmp/zhttp-all.log"

[logging.modules.zco]
level = "debug"
async = true
format = "%m%n"

[logging.modules.znet]
level = "error"
async = false
sink = "file"
file = "/tmp/znet-only.log"

[logging.modules.zhttp]
level = "info"
sink = "stdout"
)");

    const zhttp::ServerConfig config =
        zhttp::ServerConfig::from_toml(config_file.path());

    EXPECT_EQ(config.log_level, "warning");
    EXPECT_FALSE(config.log_async);
    EXPECT_EQ(config.log_format, "[%p] %m%n");
    EXPECT_EQ(config.log_sink, "both");
    EXPECT_EQ(config.log_file, "/tmp/zhttp-all.log");

    EXPECT_EQ(config.zco_log.level, "debug");
    EXPECT_TRUE(config.zco_log.has_async);
    EXPECT_TRUE(config.zco_log.async);
    EXPECT_EQ(config.zco_log.format, "%m%n");

    EXPECT_EQ(config.znet_log.level, "error");
    EXPECT_TRUE(config.znet_log.has_async);
    EXPECT_FALSE(config.znet_log.async);
    EXPECT_EQ(config.znet_log.sink, "file");
    EXPECT_EQ(config.znet_log.file, "/tmp/znet-only.log");

    EXPECT_EQ(config.zhttp_log.level, "info");
    EXPECT_FALSE(config.zhttp_log.has_async);
    EXPECT_EQ(config.zhttp_log.sink, "stdout");
}

TEST(ServerConfigTest, BuilderAppliesUnifiedLoggingConfig) {
    TempTomlFile config_file(R"(
[server]
host = "127.0.0.1"
port = 18082

[threads]
count = 1

[logging]
level = "warning"
async = true
format = "[%p][%c] %m%n"
sink = "stdout"

[logging.modules.znet]
level = "error"
async = false
sink = "file"
file = "/tmp/znet-builder.log"
)");

    zhttp::HttpServerBuilder builder;
    builder.from_config(config_file.path());

    auto server = builder.build();
    ASSERT_TRUE(server);

    auto *zco_logger = zco::get_logger();
    auto *net_logger = znet::get_logger();
    auto *http_logger = zhttp::get_logger();

    ASSERT_NE(zco_logger, nullptr);
    ASSERT_NE(net_logger, nullptr);
    ASSERT_NE(http_logger, nullptr);

    EXPECT_EQ(zco_logger->get_name(), "zco_logger");
    EXPECT_EQ(net_logger->get_name(), "znet_logger");
    EXPECT_EQ(http_logger->get_name(), "zhttp_logger");

    EXPECT_NE(dynamic_cast<zlog::AsyncLogger *>(zco_logger), nullptr);
    EXPECT_EQ(dynamic_cast<zlog::AsyncLogger *>(net_logger), nullptr);
    EXPECT_NE(dynamic_cast<zlog::AsyncLogger *>(http_logger), nullptr);
}

int main(int argc, char **argv) {
    zhttp::init_logger();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
