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
#include "zhttp/http_common.h"
#include "zhttp/http_request.h"
#include "zhttp/http_response.h"
#include "zhttp/mid/auth_middleware.h"
#include "zhttp/mid/compression_middleware.h"
#include "zhttp/mid/cors_middleware.h"
#include "zhttp/mid/error_middleware.h"
#include "zhttp/mid/middleware.h"
#include "zhttp/mid/rate_limiter_middleware.h"
#include "zhttp/mid/request_body_middleware.h"
#include "zhttp/mid/security_middleware.h"
#include "zhttp/mid/session_middleware.h"
#include "zhttp/mid/static_file_middleware.h"
#include "zhttp/mid/timeout_middleware.h"
#include "zhttp/multipart.h"
#include "zhttp/route_handler.h"
#include "zhttp/router.h"
#include "zhttp/session.h"
#include "zhttp/websocket.h"
#include "zhttp/websocket_frame.h"

// 服务器：HTTP/HTTPS 服务封装与构建器。
#include "zhttp/http_server.h"
#include "zhttp/http_server_builder.h"

// 工具：守护进程、配置等辅助组件。
#include "zhttp/daemon.h"
#include "zhttp/server_config.h"

// 日志：统一日志接口与宏定义。
#include "zhttp/zhttp_logger.h"

#endif // ZHTTP_H_
