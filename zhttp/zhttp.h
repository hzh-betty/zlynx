/**
 * zhttp.h
 * zhttp 库统一头文件
 *
 * 引入该头文件后，调用方可以一次性获得 zhttp 的主要公共 API。
 * 适合示例代码、快速原型或希望减少 include 列表的场景；
 * 如果追求更精细的编译依赖，也可以按需只包含具体模块头文件。
 *
 * @author hzh-betty
 */

#ifndef ZHTTP_H_
#define ZHTTP_H_

// 核心组件：请求、响应、解析、路由、中间件等日常 Web 开发常用能力。
#include "zhttp/content/multipart.h"
#include "zhttp/http_common.h"
#include "zhttp/http_context.h"
#include "zhttp/http_request.h"
#include "zhttp/http_response.h"
#include "zhttp/middleware/auth_middleware.h"
#include "zhttp/middleware/compression_middleware.h"
#include "zhttp/middleware/cors_middleware.h"
#include "zhttp/middleware/error_middleware.h"
#include "zhttp/middleware/middleware.h"
#include "zhttp/middleware/rate_limiter_middleware.h"
#include "zhttp/middleware/request_body_middleware.h"
#include "zhttp/middleware/security_middleware.h"
#include "zhttp/middleware/session_middleware.h"
#include "zhttp/middleware/static_file_middleware.h"
#include "zhttp/middleware/timeout_middleware.h"
#include "zhttp/router/route_handler.h"
#include "zhttp/router/router.h"
#include "zhttp/session.h"
#include "zhttp/websocket/websocket_handler.h"

// 服务器：HTTP/HTTPS 服务封装与构建器。
#include "zhttp/http_server.h"
#include "zhttp/http_server_builder.h"

// 工具：守护进程、配置等辅助组件。
#include "zhttp/runtime/daemon.h"
#include "zhttp/server_config.h"

// 日志：统一日志接口与宏定义。
#include "zhttp/zhttp_logger.h"

#endif // ZHTTP_H_
