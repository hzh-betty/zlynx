#include "zlog/module_logger.h"

namespace zlog {

ModuleLogger::ModuleLogger(const std::string &name,
                           DependencyInitializer init_dependencies,
                           const std::string &formatter)
    : name_(name), formatter_(formatter), init_dependencies_(init_dependencies) {}

void ModuleLogger::init(LogLevel::value level) {
    level_.store(static_cast<int>(level), std::memory_order_release);
    if (init_dependencies_) {
        init_dependencies_(level);
    }

    LocalLoggerBuilder builder;
    builder.build_logger_name(name_.c_str());
    builder.build_logger_level(level);
    builder.build_logger_type(LoggerType::LOGGER_ASYNC);
    builder.build_logger_formatter(formatter_);
    builder.build_logger_sink<StdOutSink>();

    Logger::ptr logger = builder.build();
    LoggerManager::get_instance().upsert_logger(name_, logger);
    std::atomic_store_explicit(&logger_, logger, std::memory_order_release);
}

Logger::ptr ModuleLogger::get_logger_ptr() {
    Logger::ptr logger =
        std::atomic_load_explicit(&logger_, std::memory_order_acquire);
    if (logger) {
        return logger;
    }
    logger = LoggerManager::get_instance().get_logger(name_);
    if (logger) {
        std::atomic_store_explicit(&logger_, logger, std::memory_order_release);
        return logger;
    }
    init(LogLevel::value::INFO);
    return std::atomic_load_explicit(&logger_, std::memory_order_acquire);
}

bool ModuleLogger::should_log(LogLevel::value level) const {
    const int configured = level_.load(std::memory_order_relaxed);
    return configured < static_cast<int>(LogLevel::value::OFF) &&
           static_cast<int>(level) >= configured;
}

} // namespace zlog
