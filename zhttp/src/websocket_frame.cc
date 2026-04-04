#include "websocket_frame.h"

#include <limits>

namespace zhttp {
namespace {

bool is_known_opcode(uint8_t opcode) {
  switch (opcode) {
  case static_cast<uint8_t>(WebSocketOpcode::kContinuation):
  case static_cast<uint8_t>(WebSocketOpcode::kText):
  case static_cast<uint8_t>(WebSocketOpcode::kBinary):
  case static_cast<uint8_t>(WebSocketOpcode::kClose):
  case static_cast<uint8_t>(WebSocketOpcode::kPing):
  case static_cast<uint8_t>(WebSocketOpcode::kPong):
    return true;
  default:
    return false;
  }
}

bool is_control_opcode(const WebSocketOpcode opcode) {
  return static_cast<uint8_t>(opcode) >= 0x8;
}

uint16_t read_u16_be(const uint8_t *data) {
  return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) |
                               static_cast<uint16_t>(data[1]));
}

uint64_t read_u64_be(const uint8_t *data) {
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value = (value << 8) | static_cast<uint64_t>(data[i]);
  }
  return value;
}

void append_u16_be(std::string *out, const uint16_t value) {
  out->push_back(static_cast<char>((value >> 8) & 0xFF));
  out->push_back(static_cast<char>(value & 0xFF));
}

void append_u64_be(std::string *out, const uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out->push_back(static_cast<char>((value >> shift) & 0xFF));
  }
}

} // namespace

WebSocketFrameParser::WebSocketFrameParser(const size_t max_message_size)
    : max_message_size_(max_message_size) {}

bool WebSocketFrameParser::parse(znet::Buffer *buffer,
                                 std::vector<WebSocketFrameEvent> *events,
                                 uint16_t *close_code,
                                 std::string *error) {
  if (!buffer || !events || !close_code || !error) {
    return false;
  }

  events->clear();
  *close_code = static_cast<uint16_t>(WebSocketCloseCode::kNormalClosure);
  error->clear();

  // 该解析器按“尽可能消费完整帧”的策略工作：
  // - 数据不足时立即返回，等待下次网络数据补齐；
  // - 一旦拿到完整帧就消费并产出事件。
  while (buffer->readable_bytes() >= 2) {
    const auto *bytes =
        reinterpret_cast<const uint8_t *>(buffer->peek());
    const bool fin = (bytes[0] & 0x80) != 0;
    const uint8_t rsv = bytes[0] & 0x70;
    const uint8_t opcode_raw = bytes[0] & 0x0F;
    const bool masked = (bytes[1] & 0x80) != 0;

    if (rsv != 0) {
      *close_code = static_cast<uint16_t>(WebSocketCloseCode::kProtocolError);
      *error = "WebSocket RSV bits must be zero";
      return false;
    }

    if (!is_known_opcode(opcode_raw)) {
      *close_code = static_cast<uint16_t>(WebSocketCloseCode::kProtocolError);
      *error = "Unknown WebSocket opcode";
      return false;
    }

    const WebSocketOpcode opcode = static_cast<WebSocketOpcode>(opcode_raw);

    uint64_t payload_length = static_cast<uint64_t>(bytes[1] & 0x7F);
    size_t header_length = 2;

    if (payload_length == 126) {
      if (buffer->readable_bytes() < 4) {
        break;
      }
      payload_length = read_u16_be(bytes + 2);
      header_length = 4;
    } else if (payload_length == 127) {
      if (buffer->readable_bytes() < 10) {
        break;
      }
      if ((bytes[2] & 0x80) != 0) {
        *close_code = static_cast<uint16_t>(WebSocketCloseCode::kProtocolError);
        *error = "Invalid WebSocket payload length";
        return false;
      }
      payload_length = read_u64_be(bytes + 2);
      header_length = 10;
    }

    if (!masked) {
      *close_code = static_cast<uint16_t>(WebSocketCloseCode::kProtocolError);
      *error = "Client WebSocket frames must be masked";
      return false;
    }

    if (buffer->readable_bytes() < header_length + 4) {
      break;
    }

    if (payload_length >
        static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
      *close_code = static_cast<uint16_t>(WebSocketCloseCode::kMessageTooLarge);
      *error = "WebSocket payload exceeds supported size";
      return false;
    }

    const size_t payload_size = static_cast<size_t>(payload_length);
    const size_t frame_size = header_length + 4 + payload_size;
    if (buffer->readable_bytes() < frame_size) {
      break;
    }

    if (is_control_opcode(opcode)) {
      if (!fin) {
        *close_code = static_cast<uint16_t>(WebSocketCloseCode::kProtocolError);
        *error = "WebSocket control frames must not be fragmented";
        return false;
      }
      if (payload_length > 125) {
        *close_code = static_cast<uint16_t>(WebSocketCloseCode::kProtocolError);
        *error = "WebSocket control frame payload is too large";
        return false;
      }
    }

    std::string payload(payload_size, '\0');
    const uint8_t *mask_key = bytes + header_length;
    const uint8_t *masked_payload = bytes + header_length + 4;
    for (size_t i = 0; i < payload_size; ++i) {
      payload[i] = static_cast<char>(masked_payload[i] ^ mask_key[i % 4]);
    }

    buffer->retrieve(frame_size);

    if (opcode == WebSocketOpcode::kContinuation) {
      if (!fragmented_message_active_) {
        *close_code = static_cast<uint16_t>(WebSocketCloseCode::kProtocolError);
        *error = "Unexpected WebSocket continuation frame";
        return false;
      }

      if (payload.size() > max_message_size_ - fragmented_payload_.size()) {
        *close_code =
            static_cast<uint16_t>(WebSocketCloseCode::kMessageTooLarge);
        *error = "WebSocket fragmented message exceeds max size";
        return false;
      }

      fragmented_payload_.append(payload);
      if (fin) {
        WebSocketFrameEvent event;
        event.opcode = fragmented_message_type_ == WebSocketMessageType::kText
                           ? WebSocketOpcode::kText
                           : WebSocketOpcode::kBinary;
        event.payload = std::move(fragmented_payload_);
        events->push_back(std::move(event));

        fragmented_message_active_ = false;
        fragmented_payload_.clear();
      }
      continue;
    }

    if (opcode == WebSocketOpcode::kText || opcode == WebSocketOpcode::kBinary) {
      if (fragmented_message_active_) {
        *close_code = static_cast<uint16_t>(WebSocketCloseCode::kProtocolError);
        *error = "New WebSocket data frame started before fragmented message end";
        return false;
      }

      if (payload.size() > max_message_size_) {
        *close_code =
            static_cast<uint16_t>(WebSocketCloseCode::kMessageTooLarge);
        *error = "WebSocket message exceeds max size";
        return false;
      }

      if (fin) {
        WebSocketFrameEvent event;
        event.opcode = opcode;
        event.payload = std::move(payload);
        events->push_back(std::move(event));
      } else {
        fragmented_message_active_ = true;
        fragmented_message_type_ = opcode == WebSocketOpcode::kText
                                       ? WebSocketMessageType::kText
                                       : WebSocketMessageType::kBinary;
        fragmented_payload_ = std::move(payload);
      }
      continue;
    }

    if (opcode == WebSocketOpcode::kClose) {
      if (payload_size == 1) {
        *close_code = static_cast<uint16_t>(WebSocketCloseCode::kProtocolError);
        *error = "Invalid WebSocket close payload";
        return false;
      }

      WebSocketFrameEvent event;
      event.opcode = WebSocketOpcode::kClose;
      event.close_code =
          static_cast<uint16_t>(WebSocketCloseCode::kNormalClosure);

      if (payload_size >= 2) {
        event.close_code =
            static_cast<uint16_t>(read_u16_be(
                reinterpret_cast<const uint8_t *>(payload.data())));
        event.payload = payload.substr(2);
      }

      events->push_back(std::move(event));
      continue;
    }

    if (opcode == WebSocketOpcode::kPing || opcode == WebSocketOpcode::kPong) {
      WebSocketFrameEvent event;
      event.opcode = opcode;
      event.payload = std::move(payload);
      events->push_back(std::move(event));
      continue;
    }
  }

  return true;
}

bool build_websocket_frame(const WebSocketOpcode opcode,
                           const std::string &payload,
                           std::string *frame,
                           const bool fin) {
  if (!frame) {
    return false;
  }

  if ((opcode == WebSocketOpcode::kPing || opcode == WebSocketOpcode::kPong ||
       opcode == WebSocketOpcode::kClose) &&
      payload.size() > 125) {
    return false;
  }

  frame->clear();
  frame->reserve(payload.size() + 14);

  const uint8_t first = static_cast<uint8_t>((fin ? 0x80 : 0x00) |
                                             static_cast<uint8_t>(opcode));
  frame->push_back(static_cast<char>(first));

  if (payload.size() <= 125) {
    frame->push_back(static_cast<char>(payload.size()));
  } else if (payload.size() <= 0xFFFF) {
    frame->push_back(static_cast<char>(126));
    append_u16_be(frame, static_cast<uint16_t>(payload.size()));
  } else {
    frame->push_back(static_cast<char>(127));
    append_u64_be(frame, static_cast<uint64_t>(payload.size()));
  }

  frame->append(payload);
  return true;
}

} // namespace zhttp
