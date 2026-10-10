#include "znet/byte_buffer.h"
#include <algorithm>
#include <cstring>
#include <functional>
#include <stdexcept>

namespace znet {
ByteBuffer::ByteBuffer(size_t capacity)
    : bytes_(std::max(capacity, size_t{1})) {}

const char *ByteBuffer::find_crlf() const {
    const auto at = view().find("\r\n");
    return at == std::string_view::npos ? nullptr : peek() + at;
}

void ByteBuffer::append(std::string_view bytes) {
    append(bytes.data(), bytes.size());
}

void ByteBuffer::append(const void *bytes, size_t size) {
    if (!size)
        return;
    if (!bytes)
        throw std::invalid_argument("Null bytes with nonzero size");
    const auto *source = static_cast<const char *>(bytes);
    // An append may resize/compact the storage. Snapshot self-appends first.
    const std::less<const char *> less;
    if (!less(source, bytes_.data()) &&
        less(source, bytes_.data() + bytes_.size())) {
        if (size > static_cast<size_t>(bytes_.data() + bytes_.size() - source))
            throw std::out_of_range("Self-append exceeds buffer storage");
        const std::string copy(source, size);
        append(copy);
        return;
    }
    ensure_writable_bytes(size);
    std::memcpy(begin_write(), source, size);
    writer_ += size;
}

void ByteBuffer::retrieve(size_t size) {
    if (size >= readable_bytes())
        retrieve_all();
    else
        reader_ += size;
}

std::string ByteBuffer::retrieve_as_string(size_t size) {
    size = std::min(size, readable_bytes());
    std::string result(peek(), size);
    retrieve(size);
    return result;
}

void ByteBuffer::ensure_writable_bytes(size_t size) {
    if (size <= writable_bytes())
        return;
    const size_t readable = readable_bytes();
    if (size > bytes_.max_size() - readable)
        throw std::length_error("ByteBuffer capacity overflow");
    // Compact only when the reclaimed prefix pays for moving the live bytes.
    // Otherwise grow with slack so small consume/append cycles are amortized.
    if (reader_ >= readable && size <= bytes_.size() - readable) {
        std::memmove(bytes_.data(), peek(), readable);
    } else {
        const size_t growth =
            bytes_.size() +
            std::min(bytes_.size(), bytes_.max_size() - bytes_.size());
        std::vector<char> storage(std::max(readable + size, growth));
        std::memcpy(storage.data(), peek(), readable);
        bytes_.swap(storage);
    }
    reader_ = 0;
    writer_ = readable;
}

void ByteBuffer::has_written(size_t size) {
    if (size > writable_bytes())
        throw std::out_of_range("ByteBuffer commit exceeds writable capacity");
    writer_ += size;
}
} // namespace znet
