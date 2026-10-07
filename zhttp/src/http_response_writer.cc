#include "zhttp/internal/http_response_writer.h"

#include "zhttp/http_response.h"
#include "zhttp/zhttp_logger.h"
#include "znet/tcp_connection.h"
#include <atomic>
#include <cstdio>
#include <mutex>
#include <unordered_set>
#include <utility>
#include <vector>

namespace zhttp {
namespace detail {
bool send_all_or_fail(const znet::TcpConnection::ptr &conn, const char *data,
                      size_t length) {
    if (length == 0) {
        return true;
    }

    if (!conn) {
        return false;
    }

    return conn->send(data, length) >= 0;
}

namespace {

bool send_chunk_frame(const znet::TcpConnection::ptr &conn, const char *data,
                      size_t length) {
    // RFC 7230 分块格式：<hex-size> CRLF <chunk-data> CRLF。
    char size_line[32];
    const int size_len =
        std::snprintf(size_line, sizeof(size_line), "%zx\r\n", length);
    if (size_len <= 0 || static_cast<size_t>(size_len) >= sizeof(size_line)) {
        return false;
    }

    if (!send_all_or_fail(conn, size_line, static_cast<size_t>(size_len))) {
        return false;
    }
    if (!send_all_or_fail(conn, data, length)) {
        return false;
    }
    return send_all_or_fail(conn, "\r\n", 2);
}

bool send_chunked_body(const znet::TcpConnection::ptr &conn,
                       const HttpResponse &response) {
    if (response.has_stream_callback()) {
        // 同步流式：服务器循环“拉取”业务层生成的数据，每次产出一个 chunk。
        constexpr size_t kStreamChunkBufferSize = 8192;
        std::vector<char> stream_buffer(kStreamChunkBufferSize);
        const auto &callback = response.stream_callback();

        while (true) {
            const size_t produced =
                callback(stream_buffer.data(), stream_buffer.size());
            if (produced == 0) {
                break;
            }
            if (produced > stream_buffer.size()) {
                return false;
            }
            if (!send_chunk_frame(conn, stream_buffer.data(), produced)) {
                return false;
            }
        }
    } else if (!response.body_content().empty()) {
        // 非流式 chunked：把完整 body 当作一个 chunk 发出。
        if (!send_chunk_frame(conn, response.body_content().data(),
                              response.body_content().size())) {
            return false;
        }
    }

    // 发送终止块（size=0）和空 trailer，标记 chunked body 结束。
    return send_all_or_fail(conn, "0\r\n\r\n", 5);
}

} // 命名空间

struct HttpResponseWriter::StreamState {
    std::mutex mutex;
    std::unordered_set<int> fds;
};
HttpResponseWriter::HttpResponseWriter()
    : state_(std::make_shared<StreamState>()) {}

bool HttpResponseWriter::is_active(const znet::TcpConnection::ptr &conn) const {
    if (!conn) {
        return false;
    }

    std::lock_guard<std::mutex> guard(state_->mutex);
    return state_->fds.find(conn->fd()) != state_->fds.end();
}

void HttpResponseWriter::mark_active(int fd) {
    if (fd < 0) {
        return;
    }

    std::lock_guard<std::mutex> guard(state_->mutex);
    state_->fds.insert(fd);
}

void HttpResponseWriter::mark_finished(int fd) {
    if (fd < 0) {
        return;
    }

    std::lock_guard<std::mutex> guard(state_->mutex);
    state_->fds.erase(fd);
}

bool HttpResponseWriter::send_async(const znet::TcpConnection::ptr &conn,
                                    const HttpResponse &response) {
    if (!conn || !response.has_async_stream_callback()) {
        return false;
    }

    // 异步推送也必须先发送响应头，后续才允许逐块写 body。
    const std::string headers = response.serialize(false);
    if (!send_all_or_fail(conn, headers.data(), headers.size())) {
        return false;
    }

    const int fd = conn->fd();
    mark_active(fd);

    auto closed = std::make_shared<std::atomic<bool>>(false);
    auto write_mutex = std::make_shared<std::mutex>();
    auto finish_stream = std::make_shared<std::function<void(bool)>>();

    *finish_stream = [state = state_, conn, fd, closed,
                      write_mutex](bool send_terminal) {
        // 结束流程只允许执行一次：避免重复 close 或重复发送终止块。
        bool expected = false;
        if (!closed->compare_exchange_strong(expected, true)) {
            return;
        }

        if (send_terminal) {
            // 与 sender 共用同一把写锁，保证 chunk 帧与终止块不会交叉。
            std::lock_guard<std::mutex> guard(*write_mutex);
            if (!send_all_or_fail(conn, "0\r\n\r\n", 5)) {
                ZHTTP_LOG_WARN(
                    "Send HTTP async chunked terminal frame failed: fd={}",
                    conn ? conn->fd() : -1);
            }
        }

        if (fd >= 0) {
            std::lock_guard<std::mutex> guard(state->mutex);
            state->fds.erase(fd);
        }
        conn->shutdown();
    };

    HttpResponse::AsyncChunkSender sender =
        [conn, closed, write_mutex, finish_stream](const std::string &chunk) {
            // 空 chunk 不编码为“零长度数据块”，终止必须通过 closer 触发。
            if (chunk.empty() || closed->load(std::memory_order_acquire)) {
                return !closed->load(std::memory_order_acquire);
            }

            bool send_ok = false;
            {
                std::lock_guard<std::mutex> guard(*write_mutex);
                if (closed->load(std::memory_order_acquire)) {
                    return false;
                }
                send_ok = send_chunk_frame(conn, chunk.data(), chunk.size());
            }

            if (!send_ok) {
                (*finish_stream)(false);
                return false;
            }

            return true;
        };

    // closer 负责最终发送终止块并关闭连接。
    HttpResponse::AsyncStreamCloser closer = [finish_stream]() {
        (*finish_stream)(true);
    };

    try {
        response.async_stream_callback()(std::move(sender), std::move(closer));
    } catch (...) {
        (*finish_stream)(false);
        return false;
    }

    return true;
}

bool HttpResponseWriter::send(const std::shared_ptr<znet::TcpConnection> &conn,
                              const HttpResponse &response) {
    const bool use_chunked =
        is_body_allowed(response.status_code()) &&
        response.version() == HttpVersion::HTTP_1_1 &&
        (response.is_chunked_enabled() || response.has_stream_callback() ||
         response.has_async_stream_callback());

    if (!use_chunked) {
        // 非 chunked 路径一次性序列化并发送完整报文。
        thread_local std::string response_buffer;
        response.serialize_to(&response_buffer);
        if (conn->send(response_buffer.data(), response_buffer.size()) < 0) {
            ZHTTP_LOG_WARN("Send HTTP response failed: fd={}", conn->fd());
            return false;
        }
    } else if (response.has_async_stream_callback()) {
        // 异步 chunked 路径交给业务层推送，完成后由 close 回调结束连接。
        try {
            if (!send_async(conn, response)) {
                ZHTTP_LOG_WARN("Send HTTP async chunked response failed: fd={}",
                               conn->fd());
                return false;
            }
        } catch (const std::exception &ex) {
            ZHTTP_LOG_WARN(
                "Async chunked response handler threw: fd={}, error={}",
                conn->fd(), ex.what());
            return false;
        } catch (...) {
            ZHTTP_LOG_WARN("Async chunked response handler threw: fd={}",
                           conn->fd());
            return false;
        }

        // 若业务层已同步 close，这里会返回 false，让上层执行收尾；
        // 若仍在异步推送中，则返回 true，等待 close 回调主动关连接。
        return is_active(conn);
    } else {
        // chunked 路径先发响应头，再发送分块体与终止块。
        const std::string headers = response.serialize(false);
        if (!send_all_or_fail(conn, headers.data(), headers.size()) ||
            !send_chunked_body(conn, response)) {
            ZHTTP_LOG_WARN("Send HTTP chunked response failed: fd={}",
                           conn->fd());
            return false;
        }
    }

    ZHTTP_LOG_DEBUG("Response: {} {}", static_cast<int>(response.status_code()),
                    status_to_string(response.status_code()));

    return response.is_keep_alive();
}

} // 命名空间 detail
} // 命名空间 zhttp
