#ifndef ZHTTP_PROTOCOL_PROTOCOL_HANDLER_H_
#define ZHTTP_PROTOCOL_PROTOCOL_HANDLER_H_

#include <memory>
namespace znet {
class TcpConnection;
class Buffer;
} // namespace znet
namespace zhttp {
// HTTP 与 WebSocket 的共同连接生命周期；调度者只在回调返回后交接实例。
/** 每个连接的协议生命周期接口；协议对象仅在回调返回后被替换。 */
class ProtocolHandler {
  public:
    /** 释放协议处理器。 */
    virtual ~ProtocolHandler() = default;
    /** 协议开始工作时调用；返回 false 表示无法继续处理。 */
    virtual bool on_open() { return true; }
    /** 消费当前可读数据，不完整数据留待后续读取事件继续处理。 */
    virtual void on_data(const std::shared_ptr<znet::TcpConnection> &connection,
                         znet::Buffer &buffer) = 0;
    /** 通知底层连接关闭，释放协议状态；实现应支持重复通知。 */
    virtual void on_closed() = 0;
};
} // namespace zhttp

#endif // ZHTTP_PROTOCOL_PROTOCOL_HANDLER_H_
