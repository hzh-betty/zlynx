#pragma once
#include "zhttp/websocket/websocket_frame.h"
namespace znet {
class Buffer;
}
namespace zhttp {
// 单次最多产出一帧；半帧留在输入 Buffer，校验和解码无需解析器对象。
// 大小限制由连接协议持有，分片累积状态属于 MessageAssembler。
bool parse_websocket_frame(znet::Buffer *buffer,
                           std::vector<WebSocketFrameEvent> *events,
                           uint16_t *close_code, std::string *error,
                           size_t max_message_size);
} // namespace zhttp
