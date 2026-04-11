#ifndef ZNET_CALLBACKS_H_
#define ZNET_CALLBACKS_H_

#include "znet/buffer.h"
#include "znet/tcp_connection.h"

#include <cstddef>
#include <functional>

namespace znet {

/**
 * @brief 连接建立完成回调。
 *
 * 触发时机：TcpConnection 完成绑定并加入服务器连接表后。
 */
using ConnectionCallback = std::function<void(const TcpConnection::ptr &)>;

/**
 * @brief 连接关闭回调。
 *
 * 触发时机：读循环退出并准备释放连接资源时。
 */
using CloseCallback = std::function<void(const TcpConnection::ptr &)>;

/**
 * @brief 输出缓冲区完全刷空回调。
 */
using WriteCompleteCallback = std::function<void(const TcpConnection::ptr &)>;

/**
 * @brief 高水位回调。
 *
 * 触发时机：待发送字节数从低于阈值跨越到大于等于阈值。
 */
using HighWaterMarkCallback =
    std::function<void(const TcpConnection::ptr &, size_t)>;

/**
 * @brief 消息到达回调。
 *
 * 触发时机：TcpConnection::read 成功后，连接输入缓冲区已有可读数据。
 */
using MessageCallback =
    std::function<void(const TcpConnection::ptr &, Buffer &)>;

} // namespace znet

#endif // ZNET_CALLBACKS_H_