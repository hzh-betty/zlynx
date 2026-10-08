#include "zhttp/parser/websocket_frame_parser.h"
#include "znet/buffer.h"
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

} // namespace
bool parse_websocket_frame(znet::Buffer *buffer,
                           std::vector<WebSocketFrameEvent> *events,
                           uint16_t *close_code, std::string *error,
                           size_t max_message_size) {
    if (!buffer || !events || !close_code || !error) {
        return false;
    }

    events->clear();
    *close_code = static_cast<uint16_t>(WebSocketCloseCode::kNormalClosure);
    error->clear();

    // 每次调用只产出一帧，协议层及时处理控制事件后再解析下一帧：
    // - 数据不足时立即返回，等待下次网络数据补齐；
    // - 一旦拿到完整帧就消费并产出事件。
    while (buffer->readable_bytes() >= 2) {
        const auto *bytes = reinterpret_cast<const uint8_t *>(buffer->peek());
        const bool fin = (bytes[0] & 0x80) != 0;
        const uint8_t rsv = bytes[0] & 0x70;
        const uint8_t opcode_raw = bytes[0] & 0x0F;
        const bool masked = (bytes[1] & 0x80) != 0;

        if (rsv != 0) {
            *close_code =
                static_cast<uint16_t>(WebSocketCloseCode::kProtocolError);
            *error = "WebSocket RSV bits must be zero";
            return false;
        }

        if (!is_known_opcode(opcode_raw)) {
            *close_code =
                static_cast<uint16_t>(WebSocketCloseCode::kProtocolError);
            *error = "Unknown WebSocket opcode";
            return false;
        }

        const WebSocketOpcode opcode = static_cast<WebSocketOpcode>(opcode_raw);

        uint64_t payload_length = static_cast<uint64_t>(bytes[1] & 0x7F);
        size_t header_length = 2;

        // 处理扩展长度字段（126/127）并保持“半包即返回”语义。
        if (payload_length == 126) {
            if (buffer->readable_bytes() < 4) {
                break;
            }
            payload_length = read_u16_be(bytes + 2);
            if (payload_length < 126) {
                *close_code = 1002;
                *error = "Non-minimal frame length";
                return false;
            }
            header_length = 4;
        } else if (payload_length == 127) {
            if (buffer->readable_bytes() < 10) {
                break;
            }
            if ((bytes[2] & 0x80) != 0) {
                *close_code =
                    static_cast<uint16_t>(WebSocketCloseCode::kProtocolError);
                *error = "Invalid WebSocket payload length";
                return false;
            }
            payload_length = read_u64_be(bytes + 2);
            if (payload_length < 65536) {
                *close_code = 1002;
                *error = "Non-minimal frame length";
                return false;
            }
            header_length = 10;
        }

        if (!masked) {
            // 服务端接收客户端帧时，mask 位必须为 1。
            *close_code =
                static_cast<uint16_t>(WebSocketCloseCode::kProtocolError);
            *error = "Client WebSocket frames must be masked";
            return false;
        }

        if (buffer->readable_bytes() < header_length + 4) {
            break;
        }

        if (payload_length >
            static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
            *close_code =
                static_cast<uint16_t>(WebSocketCloseCode::kMessageTooLarge);
            *error = "WebSocket payload exceeds supported size";
            return false;
        }

        const size_t payload_size = static_cast<size_t>(payload_length);
        if (payload_size > max_message_size ||
            payload_size >
                std::numeric_limits<size_t>::max() - header_length - 4) {
            *close_code =
                static_cast<uint16_t>(WebSocketCloseCode::kMessageTooLarge);
            *error = "WebSocket frame exceeds max size";
            return false;
        }
        if (is_control_opcode(opcode) && (!fin || payload_size > 125)) {
            *close_code =
                static_cast<uint16_t>(WebSocketCloseCode::kProtocolError);
            *error = "Invalid WebSocket control frame";
            return false;
        }
        const size_t frame_size = header_length + 4 + payload_size;
        if (buffer->readable_bytes() < frame_size) {
            break;
        }

        std::string payload(payload_size, '\0');
        const uint8_t *mask_key = bytes + header_length;
        const uint8_t *masked_payload = bytes + header_length + 4;
        for (size_t i = 0; i < payload_size; ++i) {
            payload[i] = static_cast<char>(masked_payload[i] ^ mask_key[i % 4]);
        }

        buffer->retrieve(frame_size);

        if (opcode == WebSocketOpcode::kContinuation ||
            opcode == WebSocketOpcode::kText ||
            opcode == WebSocketOpcode::kBinary) {
            events->push_back(
                {fin, opcode, std::move(payload),
                 static_cast<uint16_t>(WebSocketCloseCode::kNormalClosure)});
            break;
        }

        if (opcode == WebSocketOpcode::kClose) {
            if (payload_size == 1) {
                *close_code =
                    static_cast<uint16_t>(WebSocketCloseCode::kProtocolError);
                *error = "Invalid WebSocket close payload";
                return false;
            }

            WebSocketFrameEvent event;
            event.opcode = WebSocketOpcode::kClose;
            event.close_code =
                static_cast<uint16_t>(WebSocketCloseCode::kNormalClosure);

            if (payload_size >= 2) {
                event.close_code = static_cast<uint16_t>(read_u16_be(
                    reinterpret_cast<const uint8_t *>(payload.data())));
                if (!valid_websocket_close_code(event.close_code)) {
                    *close_code = static_cast<uint16_t>(
                        WebSocketCloseCode::kProtocolError);
                    *error = "Invalid WebSocket close status code";
                    return false;
                }

                event.payload = payload.substr(2);
                // 关闭原因按规范必须是 UTF-8 字符串。
                if (!event.payload.empty() &&
                    !valid_websocket_utf8(event.payload)) {
                    *close_code = static_cast<uint16_t>(
                        WebSocketCloseCode::kInvalidFramePayloadData);
                    *error = "WebSocket close reason is not valid UTF-8";
                    return false;
                }
            }

            events->push_back(std::move(event));
            break;
        }

        if (opcode == WebSocketOpcode::kPing ||
            opcode == WebSocketOpcode::kPong) {
            WebSocketFrameEvent event;
            event.opcode = opcode;
            event.payload = std::move(payload);
            events->push_back(std::move(event));
            break;
        }
    }

    return true;
}

} // namespace zhttp
