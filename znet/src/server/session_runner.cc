#include "session_runner.h"
#include "server_error.h"

namespace znet::detail {

void serve_session(const Connection::ptr &connection,
                   const SessionFactory &factory, const ServerOptions &options,
                   const std::atomic<bool> &running) {
    SessionCallbacks callbacks;
    bool established = false;
    try {
        // 连接启动和 TLS 握手可能阻塞，使用 handshake_timeout 限制等待时间。
        const auto handshake =
            options.handshake_timeout.count()
                ? zco::Deadline::after(options.handshake_timeout)
                : zco::Deadline{};
        auto started = connection->start(handshake);
        
        // 如果连接启动失败，且运行状态为真，且错误不是操作取消，则报告错误。
        if (!started) {
            if (running &&
                started.error().code != std::errc::operation_canceled)
                report_error(options.on_error, started.error());
        } else if (running) {
            callbacks = factory(connection);
            established = true; // 工厂已经正常返回，会话回调已经建立。
            ByteBuffer input;
            auto idle = options.idle_timeout.count()
                            ? zco::Deadline::after(options.idle_timeout)
                            : zco::Deadline{};
            while (running && connection->connected()) {
                auto deadline = options.read_timeout.count()
                                    ? zco::Deadline::after(options.read_timeout)
                                    : zco::Deadline{};

                // 单次读取与累计空闲采用较早的截止时间；零进度的读取超时不重置空闲期限。
                if (idle.time() < deadline.time())
                    deadline = idle;
                auto received =
                    connection->read(input, options.read_chunk_size, deadline);
                if (received.bytes) {
                    idle = options.idle_timeout.count()
                               ? zco::Deadline::after(options.idle_timeout)
                               : zco::Deadline{};
                    if (callbacks.on_message)
                        callbacks.on_message(connection, input);
                    else
                        input.retrieve_all();
                }

                // 处理数据、EOF 和错误
                if (received.eof)
                    break;
                if (received.error) {
                    if (!running ||
                        connection->state() == Connection::State::closed ||
                        received.error.code == std::errc::operation_canceled)
                        break;
                    if (received.error.code == std::errc::timed_out) {
                        if (idle.expired(zco::Deadline::Clock::now()) ||
                            connection->read_deadline_expired())
                            break;
                        continue;
                    }

                    // 其余传输错误均终止会话，不忙循环重试，
                    // 也不由下层重复记录日志。
                    report_error(options.on_error, received.error);
                    break;
                }
                if (!received.bytes) //避免底层异常返回导致无进展的忙循环
                    throw std::logic_error(
                        "Transport read made no progress without EOF");
            }
        }
    } catch (const std::exception &error) {
        report_error(options.on_error, callback_error(error.what()));
    } catch (...) {
        report_error(options.on_error, callback_error("unknown exception"));
    }

    // 关闭连接并通知回调；如果连接已经关闭，close() 直接返回成功。
    (void)connection->close();
    if (established && callbacks.on_close) {
        try {
            callbacks.on_close(connection);
        } catch (const std::exception &error) {
            report_error(options.on_error, callback_error(error.what()));
        } catch (...) {
            report_error(options.on_error,
                         callback_error("unknown close exception"));
        }
    }
}
} // namespace znet::detail
