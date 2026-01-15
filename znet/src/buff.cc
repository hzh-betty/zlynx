#include "buff.h"
#include "allocator.h"
#include "znet_endian.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cstring>
#include <errno.h>
#include <sys/uio.h>
#include <unistd.h>

namespace znet {

const size_t Buffer::kCheapPrepend;
const size_t Buffer::kInitialSize;

Buffer::Buffer(size_t initial_size)
    : buffer_(nullptr), capacity_(kCheapPrepend + initial_size),
      reader_index_(kCheapPrepend), writer_index_(kCheapPrepend) {
  buffer_ = static_cast<char *>(Allocator::allocate(capacity_));
  if (!buffer_) {
    throw std::bad_alloc();
  }
}

Buffer::~Buffer() {
  if (buffer_) {
    Allocator::deallocate(buffer_, capacity_);
    buffer_ = nullptr;
  }
}

size_t Buffer::read(void *data, size_t len) {
  size_t readable = readable_bytes();
  if (len > readable) {
    len = readable;
  }
  if (len > 0) {
    memcpy(data, peek(), len);
    retrieve(len);
  }
  return len;
}

int8_t Buffer::read_int8() {
  int8_t x = 0;
  read(&x, sizeof(x));
  return x;
}

int16_t Buffer::read_int16() {
  int16_t x = 0;
  read(&x, sizeof(x));
  return network_to_host(x);
}

int32_t Buffer::read_int32() {
  int32_t x = 0;
  read(&x, sizeof(x));
  return network_to_host(x);
}

int64_t Buffer::read_int64() {
  int64_t x = 0;
  read(&x, sizeof(x));
  return network_to_host(x);
}

std::string Buffer::read_string(size_t len) {
  size_t readable = readable_bytes();
  if (len > readable) {
    len = readable;
  }
  std::string str(peek(), len);
  retrieve(len);
  return str;
}

std::string Buffer::read_all_as_string() {
  return read_string(readable_bytes());
}

void Buffer::retrieve(size_t len) {
  if (len < readable_bytes()) {
    reader_index_ += len;
  } else {
    retrieve_all();
  }
}

void Buffer::retrieve_all() {
  reader_index_ = kCheapPrepend;
  writer_index_ = kCheapPrepend;
}

void Buffer::ensure_writable_bytes(size_t len) {
  if (writable_bytes() < len) {
    make_space(len);
  }
}

void Buffer::append(const void *data, size_t len) {
  ensure_writable_bytes(len);
  const char *d = static_cast<const char *>(data);
  std::copy(d, d + len, begin_write());
  has_written(len);
}

void Buffer::append(const std::string &str) { append(str.data(), str.size()); }

void Buffer::write_int8(int8_t x) { append(&x, sizeof(x)); }

void Buffer::write_int16(int16_t x) {
  int16_t be16 = host_to_network(x);
  append(&be16, sizeof(be16));
}

void Buffer::write_int32(int32_t x) {
  int32_t be32 = host_to_network(x);
  append(&be32, sizeof(be32));
}

void Buffer::write_int64(int64_t x) {
  int64_t be64 = host_to_network(x);
  append(&be64, sizeof(be64));
}

void Buffer::has_written(size_t len) { writer_index_ += len; }

const char *Buffer::find_crlf() const {
  const char crlf_pattern[] = "\r\n";
  const char *crlf =
      std::search(peek(), begin_write(), crlf_pattern, crlf_pattern + 2);
  return crlf == begin_write() ? nullptr : crlf;
}

const char *Buffer::find_eol() const {
  const char *eol = std::find(peek(), begin_write(), '\n');
  return eol == begin_write() ? nullptr : eol;
}

ssize_t Buffer::read_fd(int fd, int *saved_errno) {
  // 使用 readv 配合栈上缓冲区，减少系统调用和内存分配
  // 如果当前 buffer 可写空间不足，多余数据会读到栈缓冲区
  char extrabuf[65536];
  struct iovec vec[2];

  const size_t writable = writable_bytes();
  vec[0].iov_base = begin_write();
  vec[0].iov_len = writable;
  vec[1].iov_base = extrabuf;
  vec[1].iov_len = sizeof(extrabuf);

  // 当 buffer 可写空间 >= 64K 时，不使用栈缓冲区
  const int iovcnt = (writable < sizeof(extrabuf)) ? 2 : 1;
  const ssize_t n = ::readv(fd, vec, iovcnt);

  if (n < 0) {
    *saved_errno = errno;
  } else if (static_cast<size_t>(n) <= writable) {
    writer_index_ += n;
  } else {
    writer_index_ += writable;
    append(extrabuf, n - writable);
  }

  return n;
}

ssize_t Buffer::write_fd(int fd, int *saved_errno) {
  ssize_t n = ::write(fd, peek(), readable_bytes());
  if (n < 0) {
    *saved_errno = errno;
  } else {
    retrieve(n);
  }
  return n;
}

void Buffer::make_space(size_t len) {
  // 如果预留空间 + 可写空间 < 需要的空间，则扩容
  // 否则将数据搬移到前面，复用预留空间
  if (writable_bytes() + prependable_bytes() < len + kCheapPrepend) {
    // 需要扩容
    size_t new_capacity = writer_index_ + len;
    char *new_buffer = static_cast<char *>(Allocator::allocate(new_capacity));
    if (!new_buffer) {
      throw std::bad_alloc();
    }

    // 复制现有数据
    size_t readable = readable_bytes();
    if (readable > 0) {
      std::copy(begin() + reader_index_, begin() + writer_index_,
                new_buffer + reader_index_);
    }

    // 释放旧缓冲区
    Allocator::deallocate(buffer_, capacity_);

    // 更新指针
    buffer_ = new_buffer;
    capacity_ = new_capacity;
  } else {
    // 搬移数据
    size_t readable = readable_bytes();
    std::copy(begin() + reader_index_, begin() + writer_index_,
              begin() + kCheapPrepend);
    reader_index_ = kCheapPrepend;
    writer_index_ = reader_index_ + readable;
  }
}

void Buffer::shrink(size_t reserve) {
  size_t readable = readable_bytes();
  size_t new_capacity = kCheapPrepend + readable + reserve;

  if (new_capacity >= capacity_) {
    return; // 无需收缩
  }

  char *new_buffer = static_cast<char *>(Allocator::allocate(new_capacity));
  if (!new_buffer) {
    return; // 分配失败，不收缩
  }

  // 复制数据到新缓冲区
  if (readable > 0) {
    std::copy(begin() + reader_index_, begin() + writer_index_,
              new_buffer + kCheapPrepend);
  }

  // 释放旧缓冲区
  Allocator::deallocate(buffer_, capacity_);

  // 更新指针
  buffer_ = new_buffer;
  capacity_ = new_capacity;
  reader_index_ = kCheapPrepend;
  writer_index_ = reader_index_ + readable;
}

void Buffer::swap(Buffer &rhs) {
  std::swap(buffer_, rhs.buffer_);
  std::swap(capacity_, rhs.capacity_);
  std::swap(reader_index_, rhs.reader_index_);
  std::swap(writer_index_, rhs.writer_index_);
}

} // namespace znet
