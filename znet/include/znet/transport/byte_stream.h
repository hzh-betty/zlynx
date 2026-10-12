#ifndef ZLYNX_ZNET_INCLUDE_ZNET_TRANSPORT_BYTE_STREAM_H_
#define ZLYNX_ZNET_INCLUDE_ZNET_TRANSPORT_BYTE_STREAM_H_

#include "zco/deadline.h"
#include "znet/endpoint.h"

namespace znet {
/**
 * @brief 传输层拥有其资源；允许一个读取者和一个写入者并行操作。
 * 实现须使 close 可并发调用，并中断 start/read/write；每次调用沿用
 * 传入的绝对截止时间；通过返回值报告错误，不自行记录日志。
 */
class ByteStream {
  public:
    virtual ~ByteStream() = default;
    virtual Result<void> start(zco::Deadline deadline) = 0;
    virtual Transfer read_some(void *, size_t, zco::Deadline) = 0;
    virtual Transfer write_some(const void *, size_t, zco::Deadline) = 0;
    virtual Result<void> shutdown_write(zco::Deadline) = 0;
    virtual Result<void> close() = 0;
    virtual Result<Endpoint> local_endpoint() const = 0;
    virtual Result<Endpoint> remote_endpoint() const = 0;
    virtual int native_handle() const = 0;

    virtual bool encrypted() const { return false; }
};

} // namespace znet

#endif
