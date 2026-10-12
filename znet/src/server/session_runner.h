#ifndef ZLYNX_ZNET_SRC_SERVER_SESSION_RUNNER_H_
#define ZLYNX_ZNET_SRC_SERVER_SESSION_RUNNER_H_

#include "znet/server/tcp_server.h"
#include <atomic>

namespace znet::detail {
/**
 * @brief 在连接任务内完成握手、读取和回调，并在结束时关闭连接。
 *
 * @param factory 握手成功且仍运行时调用，生成当前会话的回调。
 * @param options 借用的超时、读取块大小和错误回调配置，须覆盖本次调用寿命。
 * @param running 共享运行标志。
 *
 * 读取结果同时携带字节和错误时先处理已收到数据；只有 factory 正常返回后，
 * 会话结束才调用 on_close，握手或创建会话失败不调用它。
 */
void serve_session(const Connection::ptr &connection,
                   const SessionFactory &factory, const ServerOptions &options,
                   const std::atomic<bool> &running);
} // namespace znet::detail

#endif
