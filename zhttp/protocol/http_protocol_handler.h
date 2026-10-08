#pragma once
#include "zhttp/parser/http_request_parser.h"
#include "zhttp/protocol/protocol_handler.h"
#include "zhttp/request_limits.h"
#include <cstdint>
#include <functional>
#include <string>
namespace zhttp {
class Router;
class RequestPipeline;
class HttpProtocolHandler : public ProtocolHandler {
  public:
    using Switch = std::function<void(std::unique_ptr<ProtocolHandler>)>;
    HttpProtocolHandler(Router &router, RequestPipeline &pipeline,
                        RequestLimits limits, uint32_t timeout,
                        std::string name, Switch switch_protocol);
    HttpProtocolHandler(const HttpProtocolHandler &) = delete;
    HttpProtocolHandler &operator=(const HttpProtocolHandler &) = delete;
    ~HttpProtocolHandler();
    void on_data(const std::shared_ptr<znet::TcpConnection> &connection,
                 znet::Buffer &buffer) override;
    void on_closed() override;

  private:
    Router &router_;
    RequestPipeline &pipeline_;
    HttpRequestParser parser_;
    uint32_t timeout_;
    std::string name_;
    Switch switch_protocol_;
    bool receiving_ = false, closed_ = false;
};
} // namespace zhttp
