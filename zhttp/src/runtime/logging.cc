#include "runtime/logging.h"
#include "zlog/logger.h"
#include <algorithm>
#include <cctype>
namespace zhttp::detail {
namespace {
zlog::LogLevel::value parse_level(std::string level) {
    std::transform(level.begin(), level.end(), level.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    using Level = zlog::LogLevel::value;
    if (level == "debug") return Level::DEBUG;
    if (level == "warning" || level == "warn") return Level::WARNING;
    if (level == "error") return Level::ERROR;
    if (level == "fatal") return Level::FATAL;
    if (level == "off") return Level::OFF;
    return Level::INFO;
}
}
HttpServer::ErrorHandler network_error_logger(const std::string &level, zlog::LogSink::ptr sink) {
    std::vector<zlog::LogSink::ptr> sinks{
        sink ? std::move(sink) : std::make_shared<zlog::StdOutSink>()};
    auto logger = std::make_shared<zlog::SyncLogger>(
        "zhttp", parse_level(level), std::make_shared<zlog::Formatter>(), sinks);
    return [logger = std::move(logger)](const znet::Error &error) {
        logger->warning(__FILE__, __LINE__, "Network session failed: {}", error.message());
    };
}
} // namespace zhttp::detail
