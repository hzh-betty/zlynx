#ifndef ZHTTP_WEBSOCKET_WEBSOCKET_MESSAGE_ASSEMBLER_H_
#define ZHTTP_WEBSOCKET_WEBSOCKET_MESSAGE_ASSEMBLER_H_

#include "zhttp/websocket/websocket_frame.h"
namespace zhttp {
// 拥有跨帧分片与累积载荷；帧解码函数不持有这些消息生命周期状态。
/** 每个连接的跨帧消息组装器，累计分片长度并校验完整文本。 */
class WebSocketMessageAssembler {
  public:
    /** 设置单条消息最大字节数。 */
    explicit WebSocketMessageAssembler(
        size_t limit = kDefaultWebSocketMaxMessageSize)
        : max_message_size_(limit) {}
    /**
     * 接收帧并向 messages 追加完整消息或控制事件。
     *
     * @param frame 已解码帧，载荷被接管。
     * @param messages 输出列表，保留已有条目。
     * @param close_code 错误时建议的关闭码。
     * @param error 失败原因。
     * @return true 表示帧被接受（消息可尚未完成），false 表示序列、长度或 UTF-8 错误。
     */
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

#endif // ZHTTP_WEBSOCKET_WEBSOCKET_MESSAGE_ASSEMBLER_H_
