#pragma once
#include "zhttp/websocket/websocket_types.h"
namespace zhttp {
struct WebSocketFrameEvent {
    bool fin = true;
    WebSocketOpcode opcode = WebSocketOpcode::kText;
    std::string payload;
    uint16_t close_code =
        static_cast<uint16_t>(WebSocketCloseCode::kNormalClosure);
};

bool build_websocket_frame(WebSocketOpcode opcode, const std::string &payload,
                           std::string *frame, bool fin = true);
bool valid_websocket_close_code(uint16_t code);
bool valid_websocket_utf8(const std::string &text);
} // namespace zhttp
