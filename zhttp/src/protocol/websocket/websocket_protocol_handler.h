#ifndef ZHTTP_PROTOCOL_WEBSOCKET_PROTOCOL_HANDLER_H_
#define ZHTTP_PROTOCOL_WEBSOCKET_PROTOCOL_HANDLER_H_

#include "protocol/websocket/websocket_frame_parser.h"
#include "protocol/protocol_handler.h"
#include "zhttp/websocket/websocket_handler.h"
#include "protocol/websocket/websocket_message_assembler.h"
#include "zhttp/websocket/websocket_types.h"
namespace zhttp {
/** WebSocket 帧读取、消息组装与生命周期通知处理器。 */
class WebSocketProtocolHandler : public ProtocolHandler {
  public:
    using ptr = std::shared_ptr<WebSocketProtocolHandler>;

    /** 绑定底层连接、只读握手请求、回调和协商参数。 */
    WebSocketProtocolHandler(std::shared_ptr<znet::Connection> connection,
                             std::shared_ptr<const HttpRequest> request,
                             WebSocketCallbacks callbacks,
                             WebSocketOptions options,
                             std::string selected_subprotocol);

    /** 触发打开回调；回调失败时通知错误并关闭连接。 */
    bool on_open() override;
    /** 按帧处理消息、ping/pong 与关闭握手。 */
    void on_data(const std::shared_ptr<znet::Connection> &connection,
                 znet::ByteBuffer &buffer) override;
    /** 标记连接关闭并最多触发一次关闭通知。 */
    void on_closed() override;

  private:
    void notify_error(const std::string &error);
    void notify_close(uint16_t close_code, const std::string &reason);

  private:
    WebSocketConnection::ptr connection_;
    std::shared_ptr<const HttpRequest> request_;
    WebSocketCallbacks callbacks_;
    // 帧解析只借用此限制；只有组装器保存跨帧的消息状态。
    size_t max_message_size_;
    WebSocketMessageAssembler assembler_;

    bool close_notified_ = false;
};

} // namespace zhttp

#endif // ZHTTP_PROTOCOL_WEBSOCKET_PROTOCOL_HANDLER_H_
