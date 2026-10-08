#pragma once
#include "zco/runtime.h"
#include <memory>
#include <vector>

namespace zco {
namespace detail {
class StackBuffer {
  public:
    explicit StackBuffer(size_t size);
    ~StackBuffer();
    StackBuffer(const StackBuffer &) = delete;
    StackBuffer &operator=(const StackBuffer &) = delete;

    char *data() const { return data_; }

    size_t size() const { return size_; }

  private:
    void *mapping_;
    char *data_;
    size_t size_, mapping_size_;
};

class StackArena {
  public:
    explicit StackArena(const RuntimeOptions &options);
    StackBuffer &shared_slot();
    std::unique_ptr<StackBuffer> acquire_independent();
    void release_independent(std::unique_ptr<StackBuffer> buffer) noexcept;

    size_t stack_size() const { return size_; }

    bool shared() const { return shared_; }

  private:
    size_t size_, next_ = 0;
    bool shared_;
    std::vector<std::unique_ptr<StackBuffer>> slots_;
    // One completed stack per worker amortizes sequential task mmap/munmap.
    // This cache has no context, callback, task identity or waiting state.
    std::unique_ptr<StackBuffer> cached_independent_;
};
} // namespace detail
} // namespace zco
