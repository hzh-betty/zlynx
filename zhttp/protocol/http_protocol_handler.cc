#include "zhttp/protocol/http_protocol_handler.h"
#include "zhttp/pipeline/request_pipeline.h"
#include "zhttp/protocol/websocket_protocol_handler.h"
#include "zhttp/websocket/websocket_handshake.h"
#include "zhttp/writer/http_response_writer.h"
#include "znet/tcp_connection.h"
namespace zhttp {
HttpProtocolHandler::HttpProtocolHandler(Router &router,
                                       RequestPipeline &pipeline,
                                       RequestLimits limits, uint32_t timeout,
                                       std::string name,
                                       Switch switch_protocol)
    : router_(router), pipeline_(pipeline), parser_(limits), timeout_(timeout),
      name_(std::move(name)), switch_protocol_(std::move(switch_protocol)) {}
HttpProtocolHandler::~HttpProtocolHandler() { on_closed(); }
void HttpProtocolHandler::on_closed() {
    if (closed_)
        return;
    closed_ = true;
    switch_protocol_ = {};
}
void HttpProtocolHandler::on_data(
    const std::shared_ptr<znet::TcpConnection> &conn, znet::Buffer &buffer) {
    if (closed_)
        return;
    auto send_error = [&](HttpStatus code, const std::string &message) {
        auto request = parser_.request();
        HttpContext context(request);
        context.response().status(code).text(message);
        context.response().set_version(request->version() ==
                                               HttpVersion::HTTP_1_0
                                           ? HttpVersion::HTTP_1_0
                                           : HttpVersion::HTTP_1_1);
        context.response().set_keep_alive(false);
        try {
            HttpResponseWriter::send(conn, context);
        } catch (...) {
        }
        context.complete(CompletionResult::Failed);
        conn->shutdown();
    };
    while (conn->connected() && !closed_) {
        if (!receiving_) {
            if (buffer.readable_bytes() == 0)
                return;
            receiving_ = true;
            // 从本次请求首字节起设定整体读取期限，半包到达时不延长期限。
            conn->set_read_deadline(timeout_);
        }
        const auto result = parser_.parse(&buffer);
        if (result == ParseResult::HEADERS_READY) {
            if (!parser_.request()->headers().get_all("Expect").empty()) {
                send_error(HttpStatus::EXPECTATION_FAILED,
                           "Unsupported Expect header");
                return;
            }
            continue;
        }
        if (result == ParseResult::NEED_MORE)
            return;
        receiving_ = false;
        conn->set_read_deadline(0);
        if (result == ParseResult::ERROR) {
            send_error(parser_.error_status(), parser_.error());
            return;
        }
        if (parser_.request()->method() == HttpMethod::CONNECT) {
            send_error(HttpStatus::NOT_IMPLEMENTED,
                       "CONNECT tunneling is unsupported");
            return;
        }
        ConnectionInfo info;
        info.tls = conn->is_tls_enabled();
        if (conn->socket()) {
            auto remote = conn->socket()->get_remote_address();
            if (remote)
                info.remote_address = remote->to_string();
            auto local = conn->socket()->get_local_address();
            if (local)
                info.local_address = local->to_string();
        }
        // 请求上下文在本次同步处理内完成，无需跨回调共享其生命周期。
        HttpContext context(parser_.request(), std::move(info));
        context.response().header("Server", name_);
        try {
            pipeline_.execute(context, router_);
        } catch (...) {
            context.reset_result();
            context.response()
                .status(HttpStatus::INTERNAL_SERVER_ERROR)
                .text("Internal Server Error");
        }
        std::unique_ptr<ProtocolHandler> candidate;
        // 业务层仅提出升级意图；握手校验成功才创建候选协议处理器。
        if (context.upgrade()) {
            auto intent = context.take_upgrade();
            std::string selected, error;
            if (prepare_websocket_handshake(context.request_ptr(),
                                            intent->options, context.response(),
                                            selected, error))
                candidate.reset(new WebSocketProtocolHandler(
                    conn, context.request_ptr(), std::move(intent->callbacks),
                    intent->options, selected));
        }
        if (!context.request().is_keep_alive() && !candidate)
            context.response().set_keep_alive(false);
        ResponsePlan plan;
        try {
            plan = HttpResponseWriter::plan(context.request().request_line(),
                                            context.response(),
                                                      static_cast<bool>(candidate));
        } catch (...) {
            candidate.reset();
            context.reset_result();
            context.response()
                .status(HttpStatus::INTERNAL_SERVER_ERROR)
                .text("Internal Server Error");
            plan = HttpResponseWriter::plan(context.request().request_line(),
                                            context.response());
        }
        WriteResult written = WriteResult::Failed;
        try {
            written = HttpResponseWriter::send(conn, context,
                                               static_cast<bool>(candidate));
        } catch (...) {
        }
        context.complete(written == WriteResult::Failed
                             ? CompletionResult::Failed
                         : candidate ? CompletionResult::Upgraded
                                     : CompletionResult::Completed);
        // 响应一旦部分写出，不能再改成错误页；直接关闭以免残留字节被当作下一响应。
        if (written == WriteResult::Failed) {
            conn->close();
            return;
        }
        if (plan.close) {
            conn->shutdown();
            return;
        }
        // 只有 101 握手完整写出后才交接协议；连接调度者在本回调返回后执行切换。
        if (candidate) {
            switch_protocol_(std::move(candidate));
            return;
        }
        // 当前响应完成且保持连接时复位解析器，继续处理缓冲区中后续请求。
        parser_.reset();
    }
}
} // namespace zhttp
