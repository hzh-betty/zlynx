#include "buffer.h"
#include <cassert>
#include <algorithm>

namespace zlog {

Buffer::Buffer() : buffer_(DEFAULT_BUFFER_SIZE), writerIdx_(0), readerIdx_(0) {}

void Buffer::push(const char *data, size_t len) {
    ensureEnoughSize(len);
    std::copy_n(data, len, &buffer_[writerIdx_]);
    moveWriter(len);
}

const char *Buffer::begin() const {
    return &buffer_[readerIdx_];
}

size_t Buffer::writeAbleSize() const {
    return (buffer_.size() - writerIdx_);
}

size_t Buffer::readAbleSize() const {
    return writerIdx_ - readerIdx_;
}

void Buffer::moveReader(size_t len) {
    assert(len <= readAbleSize());
    readerIdx_ += len;
}

void Buffer::reset() {
    readerIdx_ = writerIdx_ = 0;
}

void Buffer::swap(Buffer &buffer) noexcept {
    buffer_.swap(buffer.buffer_);
    std::swap(readerIdx_, buffer.readerIdx_);
    std::swap(writerIdx_, buffer.writerIdx_);
}

bool Buffer::empty() const {
    return readerIdx_ == writerIdx_;
}

void Buffer::ensureEnoughSize(size_t len) {
    if (len <= writeAbleSize())
        return;
    size_t newSize = 0;
    if (buffer_.size() < THRESHOLD_BUFFER_SIZE) {
        newSize = buffer_.size() * 2 + len;
    } else {
        newSize = buffer_.size() + INCREMENT_BUFFER_SIZE + len;
    }
    buffer_.resize(newSize);
}

void Buffer::moveWriter(size_t len) {
    assert(len <= writeAbleSize());
    writerIdx_ += len;
}

} // namespace zlog
