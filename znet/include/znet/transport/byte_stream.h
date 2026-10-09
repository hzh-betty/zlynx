#pragma once

#include "zco/deadline.h"
#include "znet/endpoint.h"

namespace znet {
// A transport owns its resources. One reader and one writer may run together;
// close must be thread-safe and interrupt start/read/write. Each call preserves
// the supplied absolute deadline. Implementations report errors, never log.
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
