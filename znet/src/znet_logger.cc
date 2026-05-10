#include "znet/znet_logger.h"

#include "zco/zco_log.h"

namespace znet {

namespace {

constexpr char kLoggerName[] = "znet_logger";
constexpr char kDefaultFormatter[] = "[%d{%H:%M:%S}][%c][%p]%T%m%n";

} // namespace

void init_logger(zlog::LogLevel::value level) {
    zco::init_logger(level);

    zlog::GlobalLoggerBuilder builder;
    builder.build_logger_name(kLoggerName);
    builder.build_logger_level(level);
    builder.build_logger_type(zlog::LoggerType::LOGGER_ASYNC);
    builder.build_logger_formatter(kDefaultFormatter);
    builder.build_logger_sink<zlog::StdOutSink>();

    zlog::Logger::ptr logger = builder.build();
    zlog::LoggerManager::get_instance().upsert_logger(kLoggerName, logger);
}

zlog::Logger::ptr get_logger_ptr() {
    (void)zco::get_logger_ptr();

    zlog::Logger::ptr logger =
        zlog::LoggerManager::get_instance().get_logger(kLoggerName);
    if (!logger) {
        init_logger(zlog::LogLevel::value::INFO);
        logger = zlog::LoggerManager::get_instance().get_logger(kLoggerName);
    }
    return logger;
}

} // namespace znet
