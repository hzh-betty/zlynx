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

bool is_continuation_byte(const unsigned char byte) {
    return (byte & 0xC0) == 0x80;
}

bool is_valid_utf8(const std::string &text) {
    // 严格 UTF-8 校验（RFC 3629）：
    // 1) 禁止过长编码；2) 禁止 surrogate；3) 限制到 U+10FFFF。
    const auto *bytes = reinterpret_cast<const unsigned char *>(text.data());
    const size_t size = text.size();

    size_t i = 0;
    while (i < size) {
        const unsigned char c0 = bytes[i];
        if (c0 <= 0x7F) {
            ++i;
            continue;
        }

        if (c0 >= 0xC2 && c0 <= 0xDF) {
            if (i + 1 >= size || !is_continuation_byte(bytes[i + 1])) {
                return false;
            }
            i += 2;
            continue;
        }

        if (c0 == 0xE0) {
            if (i + 2 >= size || bytes[i + 1] < 0xA0 || bytes[i + 1] > 0xBF ||
                !is_continuation_byte(bytes[i + 2])) {
                return false;
            }
            i += 3;
            continue;
        }

        if (c0 >= 0xE1 && c0 <= 0xEC) {
            if (i + 2 >= size || !is_continuation_byte(bytes[i + 1]) ||
                !is_continuation_byte(bytes[i + 2])) {
                return false;
            }
            i += 3;
            continue;
        }

        if (c0 == 0xED) {
            if (i + 2 >= size || bytes[i + 1] < 0x80 || bytes[i + 1] > 0x9F ||
                !is_continuation_byte(bytes[i + 2])) {
                return false;
            }
            i += 3;
            continue;
        }

        if (c0 >= 0xEE && c0 <= 0xEF) {
            if (i + 2 >= size || !is_continuation_byte(bytes[i + 1]) ||
                !is_continuation_byte(bytes[i + 2])) {
                return false;
            }
            i += 3;
            continue;
        }

        if (c0 == 0xF0) {
            if (i + 3 >= size || bytes[i + 1] < 0x90 || bytes[i + 1] > 0xBF ||
                !is_continuation_byte(bytes[i + 2]) ||
                !is_continuation_byte(bytes[i + 3])) {
                return false;
            }
            i += 4;
            continue;
        }

        if (c0 >= 0xF1 && c0 <= 0xF3) {
            if (i + 3 >= size || !is_continuation_byte(bytes[i + 1]) ||
                !is_continuation_byte(bytes[i + 2]) ||
                !is_continuation_byte(bytes[i + 3])) {
                return false;
            }
            i += 4;
            continue;
        }

        if (c0 == 0xF4) {
            if (i + 3 >= size || bytes[i + 1] < 0x80 || bytes[i + 1] > 0x8F ||
                !is_continuation_byte(bytes[i + 2]) ||
                !is_continuation_byte(bytes[i + 3])) {
                return false;
            }
            i += 4;
            continue;
        }

        return false;
    }

    return true;
}

bool is_valid_close_status_code(const uint16_t code) {
    // 1004/1005/1006/1015 为保留值，不能出现在线上的 Close 帧中。
    if (code < 1000 || code >= 5000) {
        return false;
    }

    if (code == 1004 || code == 1005 || code == 1006 || code == 1015) {
        return false;
    }

    return true;
}

} // namespace

WebSocketFrameParser::WebSocketFrameParser(const size_t max_message_size)
    : max_message_size_(max_message_size) {}

bool WebSocketFrameParser::parse(znet::Buffer *buffer,
                                 std::vector<WebSocketFrameEvent> *events,
                                 uint16_t *close_code, std::string *error) {
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
        const size_t frame_size = header_length + 4 + payload_size;
        if (buffer->readable_bytes() < frame_size) {
            break;
        }

        if (is_control_opcode(opcode)) {
            // 控制帧必须 FIN 且 payload <= 125。
            if (!fin) {
                *close_code =
                    static_cast<uint16_t>(WebSocketCloseCode::kProtocolError);
                *error = "WebSocket control frames must not be fragmented";
                return false;
            }
            if (payload_length > 125) {
                *close_code =
                    static_cast<uint16_t>(WebSocketCloseCode::kProtocolError);
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
                *close_code =
                    static_cast<uint16_t>(WebSocketCloseCode::kProtocolError);
                *error = "Unexpected WebSocket continuation frame";
                return false;
            }

            if (payload.size() >
                max_message_size_ - fragmented_payload_.size()) {
                *close_code =
                    static_cast<uint16_t>(WebSocketCloseCode::kMessageTooLarge);
                *error = "WebSocket fragmented message exceeds max size";
                return false;
            }

            fragmented_payload_.append(payload);
            if (fin) {
                // 分片文本消息在“最后一片”统一做 UTF-8 校验，确保跨片序列完整。
                if (fragmented_message_type_ == WebSocketMessageType::kText &&
                    !is_valid_utf8(fragmented_payload_)) {
                    *close_code = static_cast<uint16_t>(
                        WebSocketCloseCode::kInvalidFramePayloadData);
                    *error = "WebSocket text message is not valid UTF-8";
                    fragmented_message_active_ = false;
                    fragmented_payload_.clear();
                    return false;
                }

                WebSocketFrameEvent event;
                event.opcode =
                    fragmented_message_type_ == WebSocketMessageType::kText
                        ? WebSocketOpcode::kText
                        : WebSocketOpcode::kBinary;
                event.payload = std::move(fragmented_payload_);
                events->push_back(std::move(event));

                fragmented_message_active_ = false;
                fragmented_payload_.clear();
            }
            continue;
        }

        if (opcode == WebSocketOpcode::kText ||
            opcode == WebSocketOpcode::kBinary) {
            if (fragmented_message_active_) {
                *close_code =
                    static_cast<uint16_t>(WebSocketCloseCode::kProtocolError);
                *error = "New WebSocket data frame started before fragmented "
                         "message end";
                return false;
            }

            if (payload.size() > max_message_size_) {
                *close_code =
                    static_cast<uint16_t>(WebSocketCloseCode::kMessageTooLarge);
                *error = "WebSocket message exceeds max size";
                return false;
            }

            if (opcode == WebSocketOpcode::kText && fin &&
                !is_valid_utf8(payload)) {
                // 非分片文本消息在单帧路径直接校验 UTF-8。
                *close_code = static_cast<uint16_t>(
                    WebSocketCloseCode::kInvalidFramePayloadData);
                *error = "WebSocket text message is not valid UTF-8";
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
                if (!is_valid_close_status_code(event.close_code)) {
                    *close_code = static_cast<uint16_t>(
                        WebSocketCloseCode::kProtocolError);
                    *error = "Invalid WebSocket close status code";
                    return false;
                }

                event.payload = payload.substr(2);
                // 关闭原因按规范必须是 UTF-8 字符串。
                if (!event.payload.empty() && !is_valid_utf8(event.payload)) {
                    *close_code = static_cast<uint16_t>(
                        WebSocketCloseCode::kInvalidFramePayloadData);
                    *error = "WebSocket close reason is not valid UTF-8";
                    return false;
                }
            }

            events->push_back(std::move(event));
            continue;
        }

        if (opcode == WebSocketOpcode::kPing ||
            opcode == WebSocketOpcode::kPong) {
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
                           const std::string &payload, std::string *frame,
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
