#ifndef ZHTTP_H_
#define ZHTTP_H_

/**
 * @file zhttp.h
 * @brief zhttp 库统一头文件
 * @details
 * 引入该头文件后，调用方可以一次性获得 zhttp 的主要公共 API。
 * 适合示例代码、快速原型或希望减少 include 列表的场景；
 * 如果追求更精细的编译依赖，也可以按需只包含具体模块头文件。
 */

// 核心组件：请求、响应、解析、路由、中间件等日常 Web 开发常用能力。
#include "allocator.h"
#include "http_common.h"
#include "http_parser.h"
#include "http_request.h"
#include "http_response.h"
#include "middleware.h"
#include "multipart.h"
#include "rate_limiter.h"
#include "session.h"
#include "route_handler.h"
#include "router.h"

// 服务器：HTTP/HTTPS 服务封装与构建器。
#include "http_server.h"
#include "http_server_builder.h"
#include "https_server.h"
#include "ssl_context.h"

// 工具：守护进程、配置等辅助组件。
#include "daemon.h"
#include "server_config.h"

// 日志：统一日志接口与宏定义。
#include "zhttp_logger.h"

#endif // ZHTTP_H_
