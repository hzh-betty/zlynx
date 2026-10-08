#pragma once
#include "zco/deadline.h"
#include "zco/io/descriptor.h"
#include <cstddef>

namespace zco {
namespace io {
enum class Interest : unsigned { read = 1, write = 2, read_write = 3 };

struct TransferResult {
    size_t bytes = 0;
    std::error_code error;
    bool eof = false;

    explicit operator bool() const { return !error; }
};

Result<void> wait_ready(const Descriptor &descriptor, Interest interest,
                        Deadline deadline = {});
TransferResult read_some(const Descriptor &, void *, size_t, Deadline = {});
TransferResult read_exact(const Descriptor &, void *, size_t, Deadline = {});
TransferResult write_some(const Descriptor &, const void *, size_t,
                          Deadline = {});
TransferResult write_all(const Descriptor &, const void *, size_t,
                         Deadline = {});
// Reads the kernel option once when the caller explicitly requests its default.
Result<Deadline> socket_deadline(const Descriptor &, bool reading);
} // namespace io
} // namespace zco
