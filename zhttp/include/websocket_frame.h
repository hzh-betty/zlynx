#ifndef ZHTTP_WEBSOCKET_FRAME_H_
#define ZHTTP_WEBSOCKET_FRAME_H_

#include "znet/buffer.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace zhttp {

// RFC 6455 帧操作码。
enum class WebSocketOpcode : uint8_t {
  kContinuation = 0x0,
  kText = 0x1,
  kBinary = 0x2,
  kClose = 0x8,
  kPing = 0x9,
  kPong = 0xA,
};

enum class WebSocketMessageType : uint8_t {
  kText = 0x1,
  kBinary = 0x2,
};

// RFC 6455 关闭码子集：覆盖本实现会主动返回的核心错误语义。
enum class WebSocketCloseCode : uint16_t {
  kNormalClosure = 1000,
  kProtocolError = 1002,
  kUnsupportedData = 1003,
  kInvalidFramePayloadData = 1007,
  kMessageTooLarge = 1009,
  kInternalError = 1011,
};

constexpr size_t kDefaultWebSocketMaxMessageSize = 16 * 1024 * 1024;

/**
 * @brief 解析器输出事件
 * @details
 * - Text/Binary/Ping/Pong: payload 为业务负载。
 * - Close: close_code + payload(reason)。
 */
struct WebSocketFrameEvent {
  WebSocketOpcode opcode = WebSocketOpcode::kText;
  std::string payload;
  uint16_t close_code = static_cast<uint16_t>(WebSocketCloseCode::kNormalClosure);
};

/**
 * @brief WebSocket 帧解析器（服务端接收客户端帧）
 * @details
 * 该解析器维护分片重组状态，并在 parse() 里尽可能消费完整帧。
 * 若检测到协议错误，会返回 false，并通过 close_code/error 指示应回发的
 * Close 语义。
 */
class WebSocketFrameParser {
public:
  explicit WebSocketFrameParser(
      size_t max_message_size = kDefaultWebSocketMaxMessageSize);

  /**
   * @brief 解析输入缓冲区中的 WebSocket 帧
   * @param buffer 输入缓冲区，函数会消费已解析字节
   * @param events 输出事件列表
   * @param close_code 失败时建议的关闭码
   * @param error 失败时可读错误描述
   * @return true 表示当前已解析部分合法（可能因半包提前返回）
   */
  bool parse(znet::Buffer *buffer,
             std::vector<WebSocketFrameEvent> *events,
             uint16_t *close_code,
             std::string *error);

private:
  size_t max_message_size_;
  bool fragmented_message_active_ = false;
  WebSocketMessageType fragmented_message_type_ = WebSocketMessageType::kText;
  std::string fragmented_payload_;
};

/**
 * @brief 构造服务端下行帧（不加 mask）
 */
bool build_websocket_frame(WebSocketOpcode opcode,
                           const std::string &payload,
                           std::string *frame,
                           bool fin = true);

} // namespace zhttp

#endif // ZHTTP_WEBSOCKET_FRAME_H_
