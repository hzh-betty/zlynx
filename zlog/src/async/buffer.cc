/**
 * @file buffer.cc
 * @brief buffer 实现。
 * @author hzh-betty
 */

#include "buffer.h"

#include <sys/mman.h>

#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace zlog {
namespace detail {

// 预热内存：touch所有页面避免首次访问时的page fault
static void prefault_memory(char *data, size_t size) {
    // 获取页面大小，通常为4KB
    constexpr size_t PAGE_SIZE = 4096;
    // 按页遍历，触发每页的首次访问
    volatile char dummy = 0;
    for (size_t i = 0; i < size; i += PAGE_SIZE) {
        // 写入触发page fault并分配物理页
        data[i] = 0;
        dummy += data[i]; // 防止编译器优化掉写入
    }
    // 最后一页
    if (size > 0) {
        data[size - 1] = 0;
    }
    (void)dummy; // 抑制未使用警告
}

Buffer::Buffer()
    : data_(static_cast<char *>(malloc(kDefaultBufferSize))), writer_idx_(0),
      capacity_(kDefaultBufferSize) {
    if (!data_) {
        throw std::bad_alloc();
    }
    // 预热内存，避免运行时page fault
    prefault_memory(data_, capacity_);
    // 提示内核该内存将被顺序访问
    madvise(data_, capacity_, MADV_SEQUENTIAL);
}

Buffer::~Buffer() {
    if (data_) {
        free(data_);
    }
}

void Buffer::push(const char *data, size_t len) {
    ensure_enough_size(len);
    // 使用编译器内建函数进行更高效的内存拷贝
    std::memcpy(data_ + writer_idx_, data, len);
    writer_idx_ += len;
}

const char *Buffer::begin() const { return data_; }

size_t Buffer::writable_size() const { return (capacity_ - writer_idx_); }

size_t Buffer::readable_size() const { return writer_idx_; }


void Buffer::reset() { writer_idx_ = 0; }

void Buffer::swap(Buffer &buffer) noexcept {
    std::swap(data_, buffer.data_);
    std::swap(capacity_, buffer.capacity_);
    std::swap(writer_idx_, buffer.writer_idx_);
}

bool Buffer::empty() const { return writer_idx_ == 0; }

bool Buffer::can_accommodate(size_t len) const {
    if (len <= writable_size()) {
        return true;
    }
    // 计算扩容后的大小
    size_t new_size = calculate_new_size(len);
    return new_size <= kMaxBufferSize;
}

size_t Buffer::calculate_new_size(size_t len) const {
    size_t new_size = 0;
    if (capacity_ < kThresholdBufferSize) {
        new_size = capacity_ * 2 + len;
    } else {
        new_size = capacity_ + kIncrementBufferSize + len;
    }
    return new_size;
}

void Buffer::ensure_enough_size(size_t len) {
    if (len <= writable_size())
        return;
    size_t new_size = calculate_new_size(len);

    if (new_size > kMaxBufferSize) {
        new_size = kMaxBufferSize;
        if (new_size <= capacity_ ||
            (new_size - capacity_) + writable_size() < len) {
            return; // 无法扩容，保持原状
        }
    }

    char *new_data = static_cast<char *>(realloc(data_, new_size));
    if (!new_data) {
        throw std::bad_alloc();
    }

    // 预热新分配的内存区域
    if (new_size > capacity_) {
        prefault_memory(new_data + capacity_, new_size - capacity_);
        madvise(new_data, new_size, MADV_SEQUENTIAL);
    }

    data_ = new_data;
    capacity_ = new_size;
}


} // namespace detail
} // namespace zlog
