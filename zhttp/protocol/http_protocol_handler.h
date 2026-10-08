#ifndef ZHTTP_PROTOCOL_HTTP_PROTOCOL_HANDLER_H_
#define ZHTTP_PROTOCOL_HTTP_PROTOCOL_HANDLER_H_

#include "zhttp/parser/http_request_parser.h"
#include "zhttp/protocol/protocol_handler.h"
#include "zhttp/request_limits.h"
#include <cstdint>
#include <functional>
#include <string>
namespace zhttp {
class Router;
class RequestPipeline;
/** 每个连接的 HTTP 请求解析、同步分发及升级处理器。 */
class HttpProtocolHandler : public ProtocolHandler {
  public:
    /** 协议切换回调；接管候选处理器，由连接调度者延迟切换。 */
    using Switch = std::function<void(std::unique_ptr<ProtocolHandler>)>;
    /** 借用冻结的路由和流水线，绑定请求限制、读取期限及服务器名称。 */
    HttpProtocolHandler(Router &router, RequestPipeline &pipeline,
                        RequestLimits limits, uint32_t timeout,
                        std::string name, Switch switch_protocol);
    HttpProtocolHandler(const HttpProtocolHandler &) = delete;
    HttpProtocolHandler &operator=(const HttpProtocolHandler &) = delete;
    /** 关闭处理器并释放切换回调。 */
    ~HttpProtocolHandler();
    /** 逐个处理完整请求，支持 Keep-Alive、流水线请求和 WebSocket 升级。 */
    void on_data(const std::shared_ptr<znet::TcpConnection> &connection,
                 znet::Buffer &buffer) override;
    /** 幂等标记关闭并解除协议切换回调。 */
    void on_closed() override;

  private:
    Router &router_;
    RequestPipeline &pipeline_;
    HttpRequestParser parser_;
    uint32_t timeout_;
    std::string name_;
    Switch switch_protocol_;
    bool receiving_ = false, closed_ = false;
};
} // namespace zhttp

#endif // ZHTTP_PROTOCOL_HTTP_PROTOCOL_HANDLER_H_
