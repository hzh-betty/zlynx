#ifndef ZHTTP_WEBSOCKET_WEBSOCKET_TYPES_H_
#define ZHTTP_WEBSOCKET_WEBSOCKET_TYPES_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace zhttp {
/** WebSocket 帧操作码。 */
enum class WebSocketOpcode : uint8_t {
    kContinuation = 0x0,
    kText = 0x1,
    kBinary = 0x2,
    kClose = 0x8,
    kPing = 0x9,
    kPong = 0xA,
};

/** 框架常用的 WebSocket 关闭状态码。 */
enum class WebSocketCloseCode : uint16_t {
    kNormalClosure = 1000,
    kProtocolError = 1002,
    kUnsupportedData = 1003,
    kInvalidFramePayloadData = 1007,
    kMessageTooLarge = 1009,
    kInternalError = 1011,
};

/** 已组装消息的类型。 */
enum class WebSocketMessageType : uint8_t {
    kText = 0x1,
    kBinary = 0x2,
};

/** 默认单条消息上限，16 MiB。 */
constexpr size_t kDefaultWebSocketMaxMessageSize = 16 * 1024 * 1024;
/** WebSocket 协议配置；构造协议处理器时复制。 */
struct WebSocketOptions {
    /** 累计消息上限（字节）；0 使用默认上限。 */
    size_t max_message_size = kDefaultWebSocketMaxMessageSize;
    /** 服务器支持的子协议，按偏好排序。 */
    std::vector<std::string> subprotocols;
    /** 关闭握手超时，单位毫秒；0 使用默认 5000 毫秒。 */
    uint32_t close_timeout_ms = 5000;
};
} // namespace zhttp

#endif // ZHTTP_WEBSOCKET_WEBSOCKET_TYPES_H_
