/**
 * @file zhttp_logger.cc
 * @brief zhttp_logger 实现。
 * @author hzh-betty
 */

#include "zhttp/zhttp_logger.h"

#include "znet/znet_logger.h"

#include "zlog/module_logger.h"

namespace zhttp {

namespace {

zlog::ModuleLogger module_logger("zhttp_logger", znet::init_logger);

} // namespace

void init_logger(zlog::LogLevel::value level) { module_logger.init(level); }

zlog::Logger::ptr get_logger_ptr() { return module_logger.get_logger_ptr(); }

bool should_log(zlog::LogLevel::value level) {
    return module_logger.should_log(level);
}

} // namespace zhttp
