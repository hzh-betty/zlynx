#include "znet/buffer.h"

#include <algorithm>
#include <cstring>
#include <sys/uio.h>
#include <unistd.h>

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

// 使用 readv 进行“主缓冲 + 栈上临时缓冲”双段读取，减少扩容次数。
ssize_t Buffer::read_fd(int fd, int* saved_errno) {
  char extra_buf[65536];

  iovec vec[2];
  const size_t writable = writable_bytes();
  vec[0].iov_base = begin() + writer_index_;
  vec[0].iov_len = writable;
  vec[1].iov_base = extra_buf;
  vec[1].iov_len = sizeof(extra_buf);

  const int iovcnt = writable < sizeof(extra_buf) ? 2 : 1;
  const ssize_t n = ::readv(fd, vec, iovcnt);
  if (n < 0) {
    if (saved_errno) {
      *saved_errno = errno;
    }
  } else if (static_cast<size_t>(n) <= writable) {
    // 全部落在主缓冲可写区，直接推进写索引。
    writer_index_ += static_cast<size_t>(n);
  } else {
    // 主缓冲写满后，余量先落在 extra_buf，再统一 append 回主缓冲。
    writer_index_ = data_.size();
    append(extra_buf, static_cast<size_t>(n) - writable);
  }
  return n;
}

// 尽力写出当前可读区；成功后回收已写字节。
ssize_t Buffer::write_fd(int fd, int* saved_errno) {
  const ssize_t n = ::write(fd, peek(), readable_bytes());
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
