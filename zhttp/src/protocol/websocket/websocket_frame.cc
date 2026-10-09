#include "protocol/websocket/websocket_frame.h"
namespace zhttp {
bool valid_websocket_close_code(const uint16_t code) {
    // 1004/1005/1006/1015 为保留值，不能出现在线上的 Close 帧中。
    if (code < 1000 || code >= 5000) {
        return false;
    }

    if (code == 1004 || code == 1005 || code == 1006 || code == 1015) {
        return false;
    }

    return true;
}

namespace {
void append_u16_be(std::string *out, const uint16_t value) {
    out->push_back(static_cast<char>((value >> 8) & 0xFF));
    out->push_back(static_cast<char>(value & 0xFF));
}

void append_u64_be(std::string *out, const uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out->push_back(static_cast<char>((value >> shift) & 0xFF));
    }
}

bool is_continuation_byte(unsigned char b) { return (b & 0xc0) == 0x80; }
} // namespace
bool valid_websocket_utf8(const std::string &text) {
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
