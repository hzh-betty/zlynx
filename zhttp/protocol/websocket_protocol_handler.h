#pragma once
#include "zhttp/parser/websocket_frame_parser.h"
#include "zhttp/protocol/protocol_handler.h"
#include "zhttp/websocket/websocket_handler.h"
#include "zhttp/websocket/websocket_message_assembler.h"
#include "zhttp/websocket/websocket_types.h"
namespace zhttp {
class WebSocketProtocolHandler : public ProtocolHandler {
  public:
    using ptr = std::shared_ptr<WebSocketProtocolHandler>;

    WebSocketProtocolHandler(std::shared_ptr<znet::TcpConnection> connection,
                             std::shared_ptr<const HttpRequest> request,
                             WebSocketCallbacks callbacks,
                             WebSocketOptions options,
                             std::string selected_subprotocol);

    bool on_open() override;
    void on_data(const std::shared_ptr<znet::TcpConnection> &connection,
                 znet::Buffer &buffer) override;
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
    bool close_frame_sent_ = false;
};

} // namespace zhttp
