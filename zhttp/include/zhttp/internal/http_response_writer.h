#ifndef ZHTTP_INTERNAL_HTTP_RESPONSE_WRITER_H_
#define ZHTTP_INTERNAL_HTTP_RESPONSE_WRITER_H_

#include <cstddef>
#include <memory>

namespace znet {
class TcpConnection;
}
namespace zhttp {
class HttpResponse;
namespace detail {

bool send_all_or_fail(const std::shared_ptr<znet::TcpConnection> &conn,
                      const char *data, size_t length);

// 负责 HTTP 响应报文输出和流式传输的生命周期，不依赖路由逻辑。
class HttpResponseWriter {
  public:
    HttpResponseWriter();
    bool send(const std::shared_ptr<znet::TcpConnection> &conn,
              const HttpResponse &response);
    bool send_async(const std::shared_ptr<znet::TcpConnection> &conn,
                    const HttpResponse &response);
    bool is_active(const std::shared_ptr<znet::TcpConnection> &conn) const;
    void mark_active(int fd);
    void mark_finished(int fd);

  private:
    struct StreamState;
    // 异步回调只持有所需状态，不持有服务器或响应写入器。
    std::shared_ptr<StreamState> state_;
};
} // 命名空间 detail
} // 命名空间 zhttp

#endif
