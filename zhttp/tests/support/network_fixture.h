#ifndef ZHTTP_TESTS_NETWORK_FIXTURE_H_
#define ZHTTP_TESTS_NETWORK_FIXTURE_H_
#include "request_builder.h"
#include "zhttp/http_server.h"
#include "protocol/http/http_request_parser.h"
#include "protocol/websocket/websocket_frame_parser.h"
#include "protocol/websocket/websocket_protocol_handler.h"
#include "protocol/websocket/websocket_handshake.h"
#include "protocol/websocket/websocket_message_assembler.h"
#include "protocol/http/http_response_writer.h"
#include "protocol/http/response_encoder.h"
#include "znet/byte_buffer.h"
namespace zhttp {
inline bool run_server(HttpServer &server, const TestContext::ptr &context,
                       HttpResponse &response) {
    context->response() = std::move(response);
    bool found = server.handle(*context);
    response = std::move(context->response());
    return found;
}
inline ParseResult parse_request(HttpRequestParser &parser,
                                 znet::ByteBuffer *buffer) {
    auto result = parser.parse(buffer);
    return result == ParseResult::HEADERS_READY ? parser.parse(buffer) : result;
}
// Existing frame cases exercise both the newly separated parser and assembler.
class TestWebSocketParser {
  public:
    explicit TestWebSocketParser(size_t limit = kDefaultWebSocketMaxMessageSize)
        : max_message_size_(limit), assembler_(limit) {}
    bool parse(znet::ByteBuffer *buffer, std::vector<WebSocketFrameEvent> *events,
               uint16_t *code, std::string *error) {
        if (!buffer || !events || !code || !error)
            return false;
        events->clear();
        while (buffer->readable_bytes()) {
            std::vector<WebSocketFrameEvent> frames;
            if (!parse_websocket_frame(buffer, &frames, code, error,
                                       max_message_size_))
                return false;
            if (frames.empty())
                break;
            if (!assembler_.assemble(std::move(frames.front()), *events, *code,
                                     *error))
                return false;
        }
        return true;
    }

  private:
    size_t max_message_size_;
    WebSocketMessageAssembler assembler_;
};
} // namespace zhttp

namespace zhttp {
inline bool build_websocket_handshake_response(
    const std::shared_ptr<const HttpRequest> &request, std::string *output,
    std::string *error, const std::string &selected = "") {
    if (!output) {
        if (error)
            *error = "Response output is null";
        return false;
    }
    if (!error)
        return false;
    HttpResponse response;
    std::string negotiated;
    if (!prepare_websocket_handshake(request, {}, response, negotiated, *error))
        return false;
    if (!selected.empty()) {
        if (!HttpHeaders::valid(selected, "")) {
            *error = "Invalid selected subprotocol";
            return false;
        }
        response.header("Sec-WebSocket-Protocol", selected);
    }
    *output = ResponseEncoder::serialize(response);
    return true;
}
} // namespace zhttp

#endif
