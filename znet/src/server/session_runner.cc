#include "session_runner.h"
#include "server_error.h"

namespace znet::detail {
void serve_session(const Connection::ptr &connection,
                   const SessionFactory &factory, const ServerOptions &options,
                   const std::atomic<bool> &running) {
    SessionCallbacks callbacks;
    bool established = false;
    try {
        const auto handshake =
            options.handshake_timeout.count()
                ? zco::Deadline::after(options.handshake_timeout)
                : zco::Deadline{};
        auto started = connection->start(handshake);
        if (!started) {
            if (running &&
                started.error().code != std::errc::operation_canceled)
                report_error(options.on_error, started.error());
        } else if (running) {
            callbacks = factory(connection);
            established = true;
            ByteBuffer input;
            auto idle = options.idle_timeout.count()
                            ? zco::Deadline::after(options.idle_timeout)
                            : zco::Deadline{};
            while (running && connection->connected()) {
                auto deadline = options.read_timeout.count()
                                    ? zco::Deadline::after(options.read_timeout)
                                    : zco::Deadline{};
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
                    // Every other transport error is terminal; no busy-loop
                    // retry or duplicate logging by the lower layers.
                    report_error(options.on_error, received.error);
                    break;
                }
                if (!received.bytes)
                    throw std::logic_error(
                        "Transport read made no progress without EOF");
            }
        }
    } catch (const std::exception &error) {
        report_error(options.on_error, callback_error(error.what()));
    } catch (...) {
        report_error(options.on_error, callback_error("unknown exception"));
    }
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
