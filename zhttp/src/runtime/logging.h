#ifndef ZHTTP_RUNTIME_LOGGING_H_
#define ZHTTP_RUNTIME_LOGGING_H_
#include "zhttp/http_server.h"
#include "zlog/sink.h"
namespace zhttp::detail {
/** 构造独立同步日志器的错误接收回调；不注册全局日志器或创建线程。 */
HttpServer::ErrorHandler network_error_logger(const std::string &level,
                                             zlog::LogSink::ptr sink = {});
}
#endif
