/**
 * @file zco_logger.cc
 * @brief zco_logger 实现。
 * @author hzh-betty
 */

#include "zco/zco_logger.h"

#include "zlog/module_logger.h"

namespace zco {

namespace {

zlog::ModuleLogger module_logger(kLoggerName, nullptr, kDefaultFormatter);

} // namespace

void init_logger(zlog::LogLevel::value level) {
    module_logger.init(level);
}

zlog::Logger::ptr get_logger_ptr() {
    return module_logger.get_logger_ptr();
}

bool should_log(zlog::LogLevel::value level) {
    return module_logger.should_log(level);
}

} // namespace zco
