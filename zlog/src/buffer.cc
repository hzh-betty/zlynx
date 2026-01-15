#include "buffer.h"

#include <sys/mman.h>

#include <cassert>
#include <cstdlib>
#include <cstring>

#include <stdexcept>

namespace zlog {

// 预热内存：touch所有页面避免首次访问时的page fault
static void prefaultMemory(char *data, size_t size) {
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
    : data_(static_cast<char *>(std::malloc(DEFAULT_BUFFER_SIZE))),
      writerIdx_(0), capacity_(DEFAULT_BUFFER_SIZE), readerIdx_(0) {
  if (!data_) {
    throw std::bad_alloc();
  }
  // 预热内存，避免运行时page fault
  prefaultMemory(data_, capacity_);
#ifdef __linux__
  // 提示内核该内存将被顺序访问
  madvise(data_, capacity_, MADV_SEQUENTIAL);
#endif
}

Buffer::~Buffer() {
  if (data_) {
    std::free(data_);
  }
}

void Buffer::push(const char *data, size_t len) {
  ensureEnoughSize(len);
  // 使用编译器内建函数进行更高效的内存拷贝
  __builtin_memcpy(data_ + writerIdx_, data, len);
  writerIdx_ += len;
}

const char *Buffer::begin() const { return data_ + readerIdx_; }

size_t Buffer::writeAbleSize() const { return (capacity_ - writerIdx_); }

size_t Buffer::readAbleSize() const { return writerIdx_ - readerIdx_; }

void Buffer::moveReader(size_t len) {
  assert(len <= readAbleSize());
  readerIdx_ += len;
}

void Buffer::reset() { readerIdx_ = writerIdx_ = 0; }

void Buffer::swap(Buffer &buffer) noexcept {
  std::swap(data_, buffer.data_);
  std::swap(capacity_, buffer.capacity_);
  std::swap(readerIdx_, buffer.readerIdx_);
  std::swap(writerIdx_, buffer.writerIdx_);
}

bool Buffer::empty() const { return readerIdx_ == writerIdx_; }

bool Buffer::canAccommodate(size_t len) const {
  if (len <= writeAbleSize()) {
    return true;
  }
  // 计算扩容后的大小
  size_t newSize = calculateNewSize(len);
  return newSize <= MAX_BUFFER_SIZE;
}

size_t Buffer::calculateNewSize(size_t len) const {
  size_t newSize = 0;
  if (capacity_ < THRESHOLD_BUFFER_SIZE) {
    newSize = capacity_ * 2 + len;
  } else {
    newSize = capacity_ + INCREMENT_BUFFER_SIZE + len;
  }
  return newSize;
}

void Buffer::ensureEnoughSize(size_t len) {
  if (len <= writeAbleSize())
    return;
  size_t newSize = calculateNewSize(len);

  if (newSize > MAX_BUFFER_SIZE) {
    newSize = MAX_BUFFER_SIZE;
    if (newSize <= capacity_ || (newSize - capacity_) + writeAbleSize() < len) {
      return; // 无法扩容，保持原状
    }
  }

  char *newData = static_cast<char *>(std::realloc(data_, newSize));
  if (!newData) {
    throw std::bad_alloc();
  }

  // 预热新分配的内存区域
  if (newSize > capacity_) {
    prefaultMemory(newData + capacity_, newSize - capacity_);
#ifdef __linux__
    madvise(newData, newSize, MADV_SEQUENTIAL);
#endif
  }

  data_ = newData;
  capacity_ = newSize;
}

void Buffer::moveWriter(size_t len) {
  assert(len <= writeAbleSize());
  writerIdx_ += len;
}

} // namespace zlog
