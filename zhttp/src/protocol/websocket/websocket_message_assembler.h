#ifndef ZHTTP_WEBSOCKET_WEBSOCKET_MESSAGE_ASSEMBLER_H_
#define ZHTTP_WEBSOCKET_WEBSOCKET_MESSAGE_ASSEMBLER_H_

#include "protocol/websocket/websocket_frame.h"

namespace zhttp {
/**
 * @brief 每个连接的跨帧消息组装器，累计分片长度并校验完整文本。
 */
class WebSocketMessageAssembler {
  public:
    explicit WebSocketMessageAssembler(
        size_t limit = kDefaultWebSocketMaxMessageSize)
        : max_message_size_(limit) {}

    /**
     * @brief 接收帧并向 messages 追加完整消息或控制事件。
     *
     * @param frame 已解码帧，载荷被接管。
     * @param messages 输出列表，保留已有条目。
     * @param close_code 错误时建议的关闭码。
     * @param error 失败原因。
     * @return true 表示帧被接受（消息可尚未完成），false 表示序列、长度或 UTF-8
     * 错误。
     */
    bool assemble(WebSocketFrameEvent frame,
                  std::vector<WebSocketFrameEvent> &messages,
                  uint16_t &close_code, std::string &error);

  private:
    size_t max_message_size_;

    // 只跟踪尚未收到 FIN 的数据消息；中间插入控制帧不会清除此状态。
    bool fragmented_message_active_ = false;
    WebSocketMessageType fragmented_message_type_ = WebSocketMessageType::kText;

    // 单帧上限不足以约束分片消息，追加前还须检查累计字节数。
    std::string fragmented_payload_;
};
} // namespace zhttp

#endif
