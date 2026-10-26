#ifndef ZHTTP_PROTOCOL_HTTP_PROTOCOL_HANDLER_H_
#define ZHTTP_PROTOCOL_HTTP_PROTOCOL_HANDLER_H_

#include "protocol/http/http_request_parser.h"
#include "protocol/protocol_handler.h"
#include "zhttp/http_context.h"
#include "zhttp/request_limits.h"
#include <cstdint>
#include <functional>
#include <string>

namespace zhttp {
class HttpApplication;

/**
 * @brief 每个连接的 HTTP 请求解析、同步分发及升级处理器。
 */
class HttpProtocolHandler : public ProtocolHandler {
  public:
    /**
     * @brief 协议切换回调；接管候选处理器，由连接调度者延迟切换。
     */
    using Switch = std::function<void(std::unique_ptr<ProtocolHandler>)>;

    /**
     * @brief 绑定当前连接使用的应用、解析限制和协议切换入口。
     *
     * @param application 借用的已冻结应用，须覆盖处理器生命周期。
     * @param timeout 每次请求从首字节开始的整体读取期限，单位毫秒；0
     * 表示无限等待。
     * @param name 响应 Server 头的值。
     * @param switch_protocol 接管握手成功的候选协议，在当前回调返回后执行切换。
     */
    HttpProtocolHandler(const HttpApplication &application,
                        RequestLimits limits, uint32_t timeout,
                        std::string name, Switch switch_protocol);
    HttpProtocolHandler(const HttpProtocolHandler &) = delete;
    HttpProtocolHandler &operator=(const HttpProtocolHandler &) = delete;
    ~HttpProtocolHandler();

    /**
     * @brief 逐个处理完整请求，支持 Keep-Alive、流水线请求和 WebSocket 升级。
     */
    void on_data(const std::shared_ptr<znet::Connection> &connection,
                 znet::ByteBuffer &buffer) override;

    /**
     * @brief 幂等标记关闭并解除协议切换回调。
     */
    void on_closed() override;

  private:
    const HttpApplication &application_;
    HttpRequestParser parser_;
    uint32_t timeout_;
    std::string name_;
    Switch switch_protocol_;

    // receiving_ 保留跨读取事件的请求边界，防止半包到达时重新延长整体期限。
    bool receiving_ = false, closed_ = false;

    // connection_info_ 保存连接的元信息，如客户端 IP、端口等。
    bool connection_info_ready_ = false;
    ConnectionInfo connection_info_;
};
} // namespace zhttp

#endif
