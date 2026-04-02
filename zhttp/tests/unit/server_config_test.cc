#include "http_server_builder.h"
#include "server_config.h"
#include "zhttp_logger.h"

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

  auto &ref = builder.exception_handler(
      [](const zhttp::HttpRequest::ptr &, zhttp::HttpResponse &resp,
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
format = "[%p] %m%n"
sink = "both"
file = "/tmp/zhttp-all.log"

[logging.modules.zcoroutine]
level = "debug"
format = "%m%n"

[logging.modules.znet]
level = "error"
sink = "file"
file = "/tmp/znet-only.log"

[logging.modules.zhttp]
level = "info"
sink = "stdout"
)");

  const zhttp::ServerConfig config =
      zhttp::ServerConfig::from_toml(config_file.path());

  EXPECT_EQ(config.log_level, "warning");
  EXPECT_EQ(config.log_format, "[%p] %m%n");
  EXPECT_EQ(config.log_sink, "both");
  EXPECT_EQ(config.log_file, "/tmp/zhttp-all.log");

  EXPECT_EQ(config.zcoroutine_log.level, "debug");
  EXPECT_EQ(config.zcoroutine_log.format, "%m%n");

  EXPECT_EQ(config.znet_log.level, "error");
  EXPECT_EQ(config.znet_log.sink, "file");
  EXPECT_EQ(config.znet_log.file, "/tmp/znet-only.log");

  EXPECT_EQ(config.zhttp_log.level, "info");
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
format = "[%p][%c] %m%n"
sink = "stdout"

[logging.modules.znet]
level = "error"
sink = "file"
file = "/tmp/znet-builder.log"
)");

  zhttp::HttpServerBuilder builder;
  builder.from_config(config_file.path());

  auto server = builder.build();
  ASSERT_TRUE(server);

  const auto znet_config = zlog::resolve_logger_config("znet");
  EXPECT_EQ(znet_config.level, zlog::LogLevel::value::ERROR);
  EXPECT_EQ(znet_config.sink_mode, zlog::LogSinkMode::kFile);
  EXPECT_EQ(znet_config.file_path, "/tmp/znet-builder.log");

  const auto zhttp_config = zlog::resolve_logger_config("zhttp");
  EXPECT_EQ(zhttp_config.level, zlog::LogLevel::value::WARNING);
  EXPECT_EQ(zhttp_config.formatter, "[%p][%c] %m%n");
  EXPECT_EQ(zhttp_config.sink_mode, zlog::LogSinkMode::kStdout);
}

int main(int argc, char **argv) {
  zhttp::init_logger();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}