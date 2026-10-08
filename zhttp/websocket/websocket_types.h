#pragma once
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

enum class WebSocketCloseCode : uint16_t {
    kNormalClosure = 1000,
    kProtocolError = 1002,
    kUnsupportedData = 1003,
    kInvalidFramePayloadData = 1007,
    kMessageTooLarge = 1009,
    kInternalError = 1011,
};

enum class WebSocketMessageType : uint8_t {
    kText = 0x1,
    kBinary = 0x2,
};

constexpr size_t kDefaultWebSocketMaxMessageSize = 16 * 1024 * 1024;
struct WebSocketOptions {
    size_t max_message_size = kDefaultWebSocketMaxMessageSize;
    std::vector<std::string> subprotocols;
    uint32_t close_timeout_ms = 5000;
};
} // namespace zhttp
