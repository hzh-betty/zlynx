#include "execution/stack_arena.h"
#include <cerrno>
#include <limits>
#include <sys/mman.h>
#include <unistd.h>

namespace zco {
namespace detail {
StackBuffer::StackBuffer(size_t size) {
    auto page = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
    if (size < 16 * 1024 ||
        size > std::numeric_limits<size_t>::max() - 3 * page)
        throw std::invalid_argument("Invalid coroutine stack size");
    size_ = (size + page - 1) / page * page;
    mapping_size_ = size_ + 2 * page;
    mapping_ = ::mmap(nullptr, mapping_size_, PROT_NONE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping_ == MAP_FAILED)
        throw std::system_error(errno, std::generic_category(), "mmap stack");
    data_ = static_cast<char *>(mapping_) + page;
    if (::mprotect(data_, size_, PROT_READ | PROT_WRITE) != 0) {
        auto error = errno;
        ::munmap(mapping_, mapping_size_);
        throw std::system_error(error, std::generic_category(),
                                "mprotect stack");
    }
}

StackBuffer::~StackBuffer() { ::munmap(mapping_, mapping_size_); }

StackArena::StackArena(const RuntimeOptions &options)
    : size_(options.stack_size),
      shared_(options.stack_model == StackModel::kShared) {
    if (shared_) {
        if (!options.shared_stack_count)
            throw std::invalid_argument("Shared stack count must be positive");
        for (size_t i = 0; i < options.shared_stack_count; ++i)
            slots_.emplace_back(new StackBuffer(size_));
    } else {
        cached_independent_.reset(new StackBuffer(size_));
    }
}

StackBuffer &StackArena::shared_slot() {
    return *slots_.at(next_++ % slots_.size());
}

std::unique_ptr<StackBuffer> StackArena::acquire_independent() {
    if (cached_independent_)
        return std::move(cached_independent_);
    return std::unique_ptr<StackBuffer>(new StackBuffer(size_));
}

void StackArena::release_independent(
    std::unique_ptr<StackBuffer> buffer) noexcept {
    if (!cached_independent_)
        cached_independent_ = std::move(buffer);
}
} // namespace detail
} // namespace zco
