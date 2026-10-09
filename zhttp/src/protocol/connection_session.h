#ifndef ZHTTP_PROTOCOL_CONNECTION_SESSION_H_
#define ZHTTP_PROTOCOL_CONNECTION_SESSION_H_
#include "zhttp/http_application.h"
#include "zhttp/request_limits.h"
#include "znet/server/tcp_server.h"
namespace zhttp::detail {
struct HttpProtocolOptions {
    RequestLimits limits;
    uint32_t request_timeout = 30000;
    std::string server_name = "zhttp/1.0";
};
/** 会话闭包共享只读应用和协议配置；每连接唯一拥有当前及候选协议。 */
znet::SessionFactory http_sessions(std::shared_ptr<const HttpApplication> application,
                                    HttpProtocolOptions options);
} // namespace zhttp::detail
#endif
