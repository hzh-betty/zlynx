#ifndef ZHTTP_WEBSOCKET_WEBSOCKET_FRAME_H_
#define ZHTTP_WEBSOCKET_WEBSOCKET_FRAME_H_

#include "zhttp/websocket/websocket_types.h"

/*
 * WebSocket Frame 数据帧格式（RFC 6455）
 *
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-------+-+-------------+-------------------------------+
 * |F|R|R|R| opcode|M| Payload len | Extended payload length       |
 * |I|S|S|S|  (4)  |A|     (7)     |          (16/64)              |
 * |N|V|V|V|       |S|             | (if payload len == 126/127)   |
 * +-+-+-+-+-------+-+-------------+-------------------------------+
 * | Extended payload length continued, if payload len == 127      |
 * +---------------------------------------------------------------+
 * | Masking-key, if MASK set to 1 (32 bits)                       |
 * +---------------------------------------------------------------+
 * | Payload Data                                                  |
 * +---------------------------------------------------------------+
 *
 * 字段说明：
 *
 * 1. FIN（1 bit）
 *    - 1：当前帧是消息的最后一帧。
 *    - 0：当前消息尚未结束，后续还有数据帧。
 *
 * 2. RSV1 / RSV2 / RSV3（各 1 bit）
 *    - 默认必须为 0。
 *    - 协商启用扩展后，可由扩展协议定义其含义。
 *
 * 3. Opcode（4 bits）
 *    - 0x0：Continuation，连续帧。
 *    - 0x1：Text，文本消息。
 *    - 0x2：Binary，二进制消息。
 *    - 0x3 ~ 0x7：保留的数据帧操作码。
 *    - 0x8：Close，关闭连接控制帧。
 *    - 0x9：Ping，心跳探测控制帧。
 *    - 0xA：Pong，心跳响应控制帧。
 *    - 0xB ~ 0xF：保留的控制帧操作码。
 *
 * 4. MASK（1 bit）
 *    - 1：Payload 使用 Masking-Key 进行掩码处理。
 *    - 0：Payload 不进行掩码处理。
 *    - 客户端发送给服务端的帧必须 MASK = 1。
 *    - 服务端发送给客户端的帧必须 MASK = 0。
 *
 * 5. Payload length（7 bits）
 *    - 0 ~ 125：Payload 的实际长度。
 *    - 126：后续 2 字节表示实际长度（16 位无符号整数）。
 *    - 127：后续 8 字节表示实际长度（64 位无符号整数）。
 *    - 扩展长度采用网络字节序（大端序）。
 *
 * 6. Masking-Key（32 bits，可选）
 *    - 仅当 MASK = 1 时存在，占 4 字节。
 *    - Payload 中每个字节按照以下规则进行掩码：
 *        decoded[i] = encoded[i] ^ masking_key[i % 4]
 *
 * 7. Payload Data
 *    - 实际消息内容。
 *    - 文本消息必须使用 UTF-8 编码。
 *    - 消息可以由一个数据帧组成，也可以由多个数据帧分片组成。
 *    - 控制帧不能分片，Payload 长度不得超过 125 字节。
 *
 * 注意：
 *    - 基础帧头至少 2 字节。
 *    - MASK = 1 时，额外包含 4 字节 Masking-Key。
 *    - Payload length = 126 时，额外包含 2 字节长度字段。
 *    - Payload length = 127 时，额外包含 8 字节长度字段。
 *    - 控制帧必须 FIN = 1，且不能使用扩展长度编码。
 *    - 帧头中的 Payload length 不包含帧头、扩展长度字段和 Masking-Key。
 */

namespace zhttp {
/**
 * @brief 已解码的帧事件；关闭事件的 payload 为原因，close_code 为关闭码。
 */
struct WebSocketFrameEvent {
    bool fin = true;
    WebSocketOpcode opcode = WebSocketOpcode::kText;
    std::string payload;
    uint16_t close_code =
        static_cast<uint16_t>(WebSocketCloseCode::kNormalClosure);
};

/**
 * @brief 构造不带 mask 的服务端帧。
 *
 * @param opcode 帧操作码。
 * @param payload 载荷字节串；控制帧最多 125 字节。
 * @param frame 帧输出，不能为空指针。
 * @param fin 是否为最后一个分片。
 * @return 帧构造成功返回 true，输出参数为空或控制帧过大返回 false。
 */
bool build_websocket_frame(WebSocketOpcode opcode, const std::string &payload,
                           std::string *frame, bool fin = true);

/**
 * @brief 检查关闭码是否在允许范围内且不为保留值。
 */
bool valid_websocket_close_code(uint16_t code);

/**
 * @brief 校验完整 UTF-8 字节序列，拒绝过长编码、代理项和越界码点。
 */
bool valid_websocket_utf8(const std::string &text);
} // namespace zhttp

#endif
