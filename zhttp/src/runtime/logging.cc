#include "runtime/logging.h"
#include "zhttp/http_common.h"
#include "zlog/logger.h"
#include <string_view>

namespace zhttp::detail {
namespace {
zlog::LogLevel::value parse_level(std::string_view value) {
    const std::string level = to_lower(value);
    using Level = zlog::LogLevel::value;
    if (level == "debug")
        return Level::DEBUG;
    if (level == "warning" || level == "warn")
        return Level::WARNING;
    if (level == "error")
        return Level::ERROR;
    if (level == "fatal")
        return Level::FATAL;
    if (level == "off")
        return Level::OFF;
    return Level::INFO;
}
} // namespace

HttpServer::ErrorHandler network_error_logger(const std::string &level,
                                              zlog::LogSink::ptr sink) {
    std::vector<zlog::LogSink::ptr> sinks{
        sink ? std::move(sink) : std::make_shared<zlog::StdOutSink>()};
    auto logger = std::make_shared<zlog::SyncLogger>(
        "zhttp", parse_level(level), std::make_shared<zlog::Formatter>(),
        sinks);
    return [logger = std::move(logger)](const znet::Error &error) {
        logger->warning(__FILE__, __LINE__, "Network session failed: {}",
                        error.message());
    };
}
} // namespace zhttp::detail
