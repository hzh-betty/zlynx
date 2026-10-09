#ifndef ZHTTP_PARSER_WEBSOCKET_FRAME_PARSER_H_
#define ZHTTP_PARSER_WEBSOCKET_FRAME_PARSER_H_

#include "protocol/websocket/websocket_frame.h"
namespace znet {
class ByteBuffer;
}
namespace zhttp {
// 单次最多产出一帧；半帧留在输入 Buffer，校验和解码无需解析器对象。
// 大小限制由连接协议持有，分片累积状态属于 MessageAssembler。
/**
 * 校验并解码最多一个客户端帧。
 *
 * @param buffer 输入缓冲区；半帧不消费，完整帧被移除。
 * @param events 输出事件列表，每次调用先清空。
 * @param close_code 错误时建议的关闭码。
 * @param error 错误文本输出。
 * @param max_message_size 单帧载荷上限，单位字节。
 * @return true 表示无协议错误（可能尚无完整帧），false 表示错误或空输出参数。
 */
bool parse_websocket_frame(znet::ByteBuffer *buffer,
                           std::vector<WebSocketFrameEvent> *events,
                           uint16_t *close_code, std::string *error,
                           size_t max_message_size);
} // namespace zhttp

#endif // ZHTTP_PARSER_WEBSOCKET_FRAME_PARSER_H_
