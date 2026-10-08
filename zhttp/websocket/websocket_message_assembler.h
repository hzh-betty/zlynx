#pragma once
#include "zhttp/websocket/websocket_frame.h"
namespace zhttp {
// 拥有跨帧分片与累积载荷；帧解码函数不持有这些消息生命周期状态。
class WebSocketMessageAssembler {
  public:
    explicit WebSocketMessageAssembler(
        size_t limit = kDefaultWebSocketMaxMessageSize)
        : max_message_size_(limit) {}
    bool assemble(WebSocketFrameEvent frame,
                  std::vector<WebSocketFrameEvent> &messages,
                  uint16_t &close_code, std::string &error);

  private:
    size_t max_message_size_;
    bool fragmented_message_active_ = false;
    WebSocketMessageType fragmented_message_type_ = WebSocketMessageType::kText;
    std::string fragmented_payload_;
};
} // namespace zhttp
