#include "protocol/connection_session.h"
#include "protocol/http/http_protocol_handler.h"

namespace zhttp::detail {
namespace {
/**
 * @brief 单个连接的协议所有者，延迟接管升级候选以避免销毁正在执行的处理器。
 *
 * 同一会话回调串行执行；连接仅弱引用，消息及关闭回调共同持有会话。
 */
struct ConnectionSession {
    // pending 仅登记切换意图，须等 current 的 on_data 返回后才能替换。
    std::unique_ptr<ProtocolHandler> current, pending;
    std::weak_ptr<znet::Connection> connection;

    void drive(znet::ByteBuffer &buffer) {
        auto conn = connection.lock();
        if (!conn)
            return;
        current->on_data(conn, buffer);

        // 等 HTTP 回调返回后再销毁旧协议，随即消费握手后已缓冲的帧。
        if (pending && conn->connected()) {
            current = std::move(pending);
            if (current->on_open() && buffer.readable_bytes())
                current->on_data(conn, buffer);
        }
    }

    void close() {
        if (current)
            current->on_closed();
        pending.reset();
        current.reset();
    }
};
} // namespace

znet::SessionFactory
http_sessions(std::shared_ptr<const HttpApplication> application,
              HttpProtocolOptions options) {
    return [application = std::move(application),
            options = std::move(options)](const znet::Connection::ptr &conn) {
        auto session = std::make_shared<ConnectionSession>();
        session->connection = conn;
        std::weak_ptr<ConnectionSession> weak = session;
        session->current = std::make_unique<HttpProtocolHandler>(
            *application, options.limits, options.request_timeout,
            options.server_name,
            [weak](std::unique_ptr<ProtocolHandler> pending) {
                if (auto session = weak.lock())
                    session->pending = std::move(pending);
            });
        return znet::SessionCallbacks{
            [session](const znet::Connection::ptr &, znet::ByteBuffer &buffer) {
                session->drive(buffer);
            },
            [session](const znet::Connection::ptr &) { session->close(); }};
    };
}
} // namespace zhttp::detail
