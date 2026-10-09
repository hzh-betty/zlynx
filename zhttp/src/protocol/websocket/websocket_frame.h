#ifndef ZHTTP_WEBSOCKET_WEBSOCKET_FRAME_H_
#define ZHTTP_WEBSOCKET_WEBSOCKET_FRAME_H_

#include "zhttp/websocket/websocket_types.h"
namespace zhttp {
/** 已解码的帧事件；关闭事件的 payload 为原因，close_code 为关闭码。 */
struct WebSocketFrameEvent {
    bool fin = true;
    WebSocketOpcode opcode = WebSocketOpcode::kText;
    std::string payload;
    uint16_t close_code =
        static_cast<uint16_t>(WebSocketCloseCode::kNormalClosure);
};

/**
 * 构造不带 mask 的服务端帧。
 *
 * @param opcode 帧操作码。
 * @param payload 载荷字节串；控制帧最多 125 字节。
 * @param frame 帧输出，不能为空指针。
 * @param fin 是否为最后一个分片。
 * @return 帧构造成功返回 true，输出参数为空或控制帧过大返回 false。
 */
bool build_websocket_frame(WebSocketOpcode opcode, const std::string &payload,
                           std::string *frame, bool fin = true);
/** 检查关闭码是否在允许范围内且不为保留值。 */
bool valid_websocket_close_code(uint16_t code);
/** 校验完整 UTF-8 字节序列，拒绝过长编码、代理项和越界码点。 */
bool valid_websocket_utf8(const std::string &text);
} // namespace zhttp

#endif // ZHTTP_WEBSOCKET_WEBSOCKET_FRAME_H_
