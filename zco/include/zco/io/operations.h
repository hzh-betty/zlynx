#ifndef ZLYNX_ZCO_INCLUDE_ZCO_IO_OPERATIONS_H_
#define ZLYNX_ZCO_INCLUDE_ZCO_IO_OPERATIONS_H_

#include "zco/deadline.h"
#include "zco/io/descriptor.h"
#include <cstddef>

namespace zco {
namespace io {
enum class Interest : unsigned { read = 1, write = 2, read_write = 3 };

/**
 * @brief 记录已传输字节、错误和读取结束状态。
 * @note bytes 在失败时仍表示已经完成的进度；EOF 可以与空 error 同时出现。
 * 布尔转换仅检查 error，不保证已达到请求字节数，调用方还需检查 bytes 和 eof。
 */
struct TransferResult {
    size_t bytes = 0;
    std::error_code error;
    bool eof = false;

    explicit operator bool() const { return !error; }
};

/**
 * @brief 在协程任务内等待指定方向就绪；就绪通知不保证下一次系统调用一定成功。
 * @return 普通线程调用返回
 * operation_not_permitted；关闭、超时或取消返回对应错误。
 */
Result<void> wait_ready(const Descriptor &descriptor, Interest interest,
                        Deadline deadline = {});

/**
 * @brief 读取至多请求长度的字节，必要时等待就绪。
 * @pre 非零长度要求可写缓冲区覆盖请求长度，描述符须保持非阻塞模式。
 * @note 遇到需要等待的情况时要求在协程任务内调用。
 */
TransferResult read_some(const Descriptor &, void *, size_t, Deadline = {});

/**
 * @brief 累积读取，直到读满、EOF、错误或等待超时。
 * @pre 非零读取长度要求缓冲区覆盖该长度，且描述符保持非阻塞模式。
 * @return 包含部分读取进度；EOF 时 error 可以为空，bytes 可以小于请求长度。
 * @note 所有重试沿用同一个绝对截止时间，已读入的数据不会回滚。
 */
TransferResult read_exact(const Descriptor &, void *, size_t, Deadline = {});

/**
 * @brief 写入至多请求长度的字节，必要时等待就绪。
 * @pre 非零长度要求可读缓冲区覆盖请求长度，描述符须保持非阻塞模式。
 * @note 遇到需要等待的情况时要求在协程任务内调用。
 */
TransferResult write_some(const Descriptor &, const void *, size_t,
                          Deadline = {});

/**
 * @brief 累积写入，直到全部写入、错误或等待超时。
 * @pre 非零写入长度要求缓冲区覆盖该长度，且描述符保持非阻塞模式。
 * @return 包含失败前已写入的字节数；已写入的数据不会回滚。
 * @note 所有重试沿用同一个绝对截止时间。
 */
TransferResult write_all(const Descriptor &, const void *, size_t,
                         Deadline = {});

/**
 * @brief 读取一次套接字接收或发送超时选项，将其换算为当前时刻起的截止时间。
 * @param reading 为 true 时读取 SO_RCVTIMEO，否则读取 SO_SNDTIMEO。
 * @return 内核超时为零时返回无截止时间的 Deadline；读取失败时返回错误。
 */
Result<Deadline> socket_deadline(const Descriptor &, bool reading);
} // namespace io
} // namespace zco

#endif
