#ifndef ZLOG_MODULE_LOGGER_H_
#define ZLOG_MODULE_LOGGER_H_

#include "zlog/logger.h"

#include <atomic>
#include <string>

namespace zlog {

/** @brief 模块日志的级别过滤、缓存和初始化；不依赖使用它的上层模块。 */
class ModuleLogger {
  public:
    using DependencyInitializer = void (*)(LogLevel::value);

    explicit ModuleLogger(
        const std::string &name,
        DependencyInitializer init_dependencies = nullptr,
        const std::string &formatter = "[%d{%H:%M:%S}][%c][%p]%T%m%n");

    void init(LogLevel::value level);
    Logger::ptr get_logger_ptr();
    bool should_log(LogLevel::value level) const;

  private:
    const std::string name_;
    const std::string formatter_;
    DependencyInitializer init_dependencies_;
    std::atomic<int> level_{static_cast<int>(LogLevel::value::INFO)};
    Logger::ptr logger_;
};

} // namespace zlog

#endif // ZLOG_MODULE_LOGGER_H_
