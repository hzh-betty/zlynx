#ifndef ZHTTP_PROTOCOL_PROTOCOL_HANDLER_H_
#define ZHTTP_PROTOCOL_PROTOCOL_HANDLER_H_

#include <memory>

namespace znet {
class Connection;
class ByteBuffer;
} // namespace znet

namespace zhttp {
/**
 * @brief 单连接的协议生命周期接口，同一实例的回调由连接任务串行执行。
 *
 * 协议对象仅在回调返回后替换；输入缓冲区借用到本次 on_data 返回，
 * 实现可消费已完成部分并将后续解析进度保存在自身状态中。
 */
class ProtocolHandler {
  public:
    virtual ~ProtocolHandler() = default;

    /**
     * @brief 协议开始工作时调用；返回 false 表示无法继续处理。
     */
    virtual bool on_open() { return true; }

    /**
     * @brief 消费当前可读数据，不完整数据留待后续读取事件继续处理。
     */
    virtual void on_data(const std::shared_ptr<znet::Connection> &connection,
                         znet::ByteBuffer &buffer) = 0;

    /**
     * @brief 通知底层连接关闭，释放协议状态；实现应支持重复通知。
     */
    virtual void on_closed() = 0;
};
} // namespace zhttp

#endif
