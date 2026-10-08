#include "zhttp/websocket/websocket_message_assembler.h"
namespace zhttp {
bool WebSocketMessageAssembler::assemble(
    WebSocketFrameEvent frame, std::vector<WebSocketFrameEvent> &messages,
    uint16_t &close_code, std::string &error) {
    const auto opcode = frame.opcode;
    const bool fin = frame.fin;
    auto payload = std::move(frame.payload);
    if (opcode == WebSocketOpcode::kContinuation) {
        if (!fragmented_message_active_) {
            close_code =
                static_cast<uint16_t>(WebSocketCloseCode::kProtocolError);
            error = "Unexpected WebSocket continuation frame";
            return false;
        }

        if (payload.size() > max_message_size_ - fragmented_payload_.size()) {
            close_code =
                static_cast<uint16_t>(WebSocketCloseCode::kMessageTooLarge);
            error = "WebSocket fragmented message exceeds max size";
            return false;
        }

        fragmented_payload_.append(payload);
        if (fin) {
            // 分片文本消息在“最后一片”统一做 UTF-8 校验，确保跨片序列完整。
            if (fragmented_message_type_ == WebSocketMessageType::kText &&
                !valid_websocket_utf8(fragmented_payload_)) {
                close_code = static_cast<uint16_t>(
                    WebSocketCloseCode::kInvalidFramePayloadData);
                error = "WebSocket text message is not valid UTF-8";
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
            messages.push_back(std::move(event));

            fragmented_message_active_ = false;
            fragmented_payload_.clear();
        }
        return true;
    }

    if (opcode == WebSocketOpcode::kText ||
        opcode == WebSocketOpcode::kBinary) {
        if (fragmented_message_active_) {
            close_code =
                static_cast<uint16_t>(WebSocketCloseCode::kProtocolError);
            error = "New WebSocket data frame started before fragmented "
                    "message end";
            return false;
        }

        if (payload.size() > max_message_size_) {
            close_code =
                static_cast<uint16_t>(WebSocketCloseCode::kMessageTooLarge);
            error = "WebSocket message exceeds max size";
            return false;
        }

        if (opcode == WebSocketOpcode::kText && fin &&
            !valid_websocket_utf8(payload)) {
            // 非分片文本消息在单帧路径直接校验 UTF-8。
            close_code = static_cast<uint16_t>(
                WebSocketCloseCode::kInvalidFramePayloadData);
            error = "WebSocket text message is not valid UTF-8";
            return false;
        }

        if (fin) {
            WebSocketFrameEvent event;
            event.opcode = opcode;
            event.payload = std::move(payload);
            messages.push_back(std::move(event));
        } else {
            fragmented_message_active_ = true;
            fragmented_message_type_ = opcode == WebSocketOpcode::kText
                                           ? WebSocketMessageType::kText
                                           : WebSocketMessageType::kBinary;
            fragmented_payload_ = std::move(payload);
        }
        return true;
    }

    frame.payload = std::move(payload);
    messages.push_back(std::move(frame));
    return true;
}
} // namespace zhttp
