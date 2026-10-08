#pragma once
#include <memory>
namespace znet {
class TcpConnection;
class Buffer;
} // namespace znet
namespace zhttp {
// HTTP 与 WebSocket 的共同连接生命周期；调度者只在回调返回后交接实例。
class ProtocolHandler {
  public:
    virtual ~ProtocolHandler() = default;
    virtual bool on_open() { return true; }
    virtual void on_data(const std::shared_ptr<znet::TcpConnection> &connection,
                         znet::Buffer &buffer) = 0;
    virtual void on_closed() = 0;
};
} // namespace zhttp
