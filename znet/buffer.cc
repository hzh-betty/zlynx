#include "znet/buffer.h"

#include <algorithm>
#include <cerrno>

#include "znet/socket.h"

namespace znet {

// 读写索引初始都指向可读区起点（即预留头部之后）。
Buffer::Buffer(size_t initial_size)
    : data_(kCheapPrepend + initial_size),
      reader_index_(kCheapPrepend),
      writer_index_(kCheapPrepend) {}

size_t Buffer::readable_bytes() const {
  return writer_index_ - reader_index_;
}

size_t Buffer::writable_bytes() const {
  return data_.size() - writer_index_;
}

size_t Buffer::prependable_bytes() const {
  return reader_index_;
}

const char* Buffer::peek() const {
  if (readable_bytes() == 0) {
    return nullptr;
  }
  return begin() + reader_index_;
}

// 消费 length 字节；若请求超出可读区则直接清空。
void Buffer::retrieve(size_t length) {
  if (length < readable_bytes()) {
    reader_index_ += length;
  } else {
    retrieve_all();
  }
}

void Buffer::retrieve_all() {
  // 保留 prepend 区间，方便后续协议头回填。
  reader_index_ = kCheapPrepend;
  writer_index_ = kCheapPrepend;
}

// 取出并消费前 n 字节（n 为请求值与可读值的较小者）。
std::string Buffer::retrieve_as_string(size_t length) {
  const size_t n = std::min(length, readable_bytes());
  std::string out;
  if (n > 0) {
    out.assign(peek(), n);
  }
  retrieve(n);
  return out;
}

std::string Buffer::retrieve_all_as_string() {
  std::string out;
  if (readable_bytes() > 0) {
    out.assign(peek(), readable_bytes());
  }
  retrieve_all();
  return out;
}

void Buffer::append(const void* data, size_t length) {
  // 与常见容器语义保持一致：空输入直接无副作用返回。
  if (!data || length == 0) {
    return;
  }
  append(static_cast<const char*>(data), length);
}

void Buffer::append(const char* data, size_t length) {
  if (!data || length == 0) {
    return;
  }
  ensure_writable_bytes(length);
  // 目标区间与源区间不重叠，使用 std::copy 简洁且安全。
  std::copy(data, data + length, begin_write());
  has_written(length);
}

void Buffer::append(const std::string& data) {
  append(data.data(), data.size());
}

char* Buffer::begin_write() {
  return begin() + writer_index_;
}

const char* Buffer::begin_write() const {
  return begin() + writer_index_;
}

void Buffer::has_written(size_t length) {
  writer_index_ += length;
}

void Buffer::ensure_writable_bytes(size_t length) {
  if (writable_bytes() < length) {
    make_space(length);
  }
}

ssize_t Buffer::read_from_socket(const std::shared_ptr<Socket>& socket,
                                 size_t max_read_bytes,
                                 uint32_t timeout_ms,
                                 int* saved_errno) {
  if (!socket || !socket->is_valid()) {
    errno = EBADF;
    if (saved_errno) {
      *saved_errno = errno;
    }
    return -1;
  }

  if (max_read_bytes == 0) {
    errno = EINVAL;
    if (saved_errno) {
      *saved_errno = errno;
    }
    return -1;
  }

  ensure_writable_bytes(max_read_bytes);
  const ssize_t n = socket->recv(begin_write(), max_read_bytes, 0, timeout_ms);
  if (n < 0) {
    if (saved_errno) {
      *saved_errno = errno;
    }
    return -1;
  }

  has_written(static_cast<size_t>(n));
  return n;
}

ssize_t Buffer::write_to_socket(const std::shared_ptr<Socket>& socket,
                                uint32_t timeout_ms,
                                int* saved_errno) {
  if (!socket || !socket->is_valid()) {
    errno = EBADF;
    if (saved_errno) {
      *saved_errno = errno;
    }
    return -1;
  }

  if (readable_bytes() == 0) {
    return 0;
  }

  const ssize_t n = socket->send(peek(), readable_bytes(), 0, timeout_ms);
  if (n < 0) {
    if (saved_errno) {
      *saved_errno = errno;
    }
    return n;
  }

  retrieve(static_cast<size_t>(n));
  return n;
}

char* Buffer::begin() {
  return data_.empty() ? nullptr : &*data_.begin();
}

const char* Buffer::begin() const {
  return data_.empty() ? nullptr : &*data_.begin();
}

void Buffer::make_space(size_t length) {
  // 若“可写 + 可前置”仍不足，直接扩容到可容纳新增数据。
  if (writable_bytes() + prependable_bytes() < length + kCheapPrepend) {
    data_.resize(writer_index_ + length);
    return;
  }

  // 否则将可读区前移到 prepend 后，复用前部空洞避免扩容。
  const size_t readable = readable_bytes();
  std::copy(begin() + reader_index_, begin() + writer_index_,
            begin() + kCheapPrepend);
  reader_index_ = kCheapPrepend;
  writer_index_ = reader_index_ + readable;
}

}  // namespace znet
