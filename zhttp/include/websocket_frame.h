#ifndef ZHTTP_WEBSOCKET_FRAME_H_
#define ZHTTP_WEBSOCKET_FRAME_H_

#include "znet/buffer.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace zhttp {

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

enum class WebSocketCloseCode : uint16_t {
  kNormalClosure = 1000,
  kProtocolError = 1002,
  kUnsupportedData = 1003,
  kMessageTooLarge = 1009,
  kInternalError = 1011,
};

constexpr size_t kDefaultWebSocketMaxMessageSize = 16 * 1024 * 1024;

struct WebSocketFrameEvent {
  WebSocketOpcode opcode = WebSocketOpcode::kText;
  std::string payload;
  uint16_t close_code = static_cast<uint16_t>(WebSocketCloseCode::kNormalClosure);
};

class WebSocketFrameParser {
public:
  explicit WebSocketFrameParser(
      size_t max_message_size = kDefaultWebSocketMaxMessageSize);

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

bool build_websocket_frame(WebSocketOpcode opcode,
                           const std::string &payload,
                           std::string *frame,
                           bool fin = true);

} // namespace zhttp

#endif // ZHTTP_WEBSOCKET_FRAME_H_
