#include "zco/zco_log.h"

#include <atomic>
#include <memory>

namespace zco {

namespace {

std::atomic<int> g_log_level{static_cast<int>(zlog::LogLevel::value::INFO)};
zlog::Logger::ptr zco_logger;

} // namespace

void init_logger(zlog::LogLevel::value level) {
    g_log_level.store(static_cast<int>(level), std::memory_order_release);

    zlog::GlobalLoggerBuilder builder;
    builder.build_logger_name(kLoggerName);
    builder.build_logger_level(level);
    builder.build_logger_type(zlog::LoggerType::LOGGER_ASYNC);
    builder.build_logger_formatter(kDefaultFormatter);
    builder.build_logger_sink<zlog::StdOutSink>();

    zlog::Logger::ptr logger = builder.build();
    zlog::LoggerManager::get_instance().upsert_logger(kLoggerName, logger);
    std::atomic_store_explicit(&zco_logger, logger, std::memory_order_release);
}

zlog::Logger::ptr get_logger_ptr() {
    zlog::Logger::ptr logger =
        std::atomic_load_explicit(&zco_logger, std::memory_order_acquire);
    if (logger) {
        return logger;
    }

    logger = zlog::LoggerManager::get_instance().get_logger(kLoggerName);
    if (logger) {
        std::atomic_store_explicit(&zco_logger, logger,
                                   std::memory_order_release);
        return logger;
    }

    init_logger(zlog::LogLevel::value::INFO);
    return std::atomic_load_explicit(&zco_logger, std::memory_order_acquire);
}

bool should_log(zlog::LogLevel::value level) {
    const int configured = g_log_level.load(std::memory_order_relaxed);
    if (configured >= static_cast<int>(zlog::LogLevel::value::OFF)) {
        return false;
    }
    return static_cast<int>(level) >= configured;
}

} // namespace zco
