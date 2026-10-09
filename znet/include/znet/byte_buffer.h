#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace znet {
// Contiguous protocol bytes, owned and mutated by one session. No IO or
// runtime dependency. Pointers/views are invalidated by append/reserve.
class ByteBuffer {
  public:
    explicit ByteBuffer(size_t capacity = 1024);
    ByteBuffer(const ByteBuffer &) = default;
    ByteBuffer &operator=(const ByteBuffer &) = default;

    size_t readable_bytes() const { return writer_ - reader_; }

    size_t writable_bytes() const { return bytes_.size() - writer_; }

    const char *peek() const { return bytes_.data() + reader_; }

    char *begin_write() { return bytes_.data() + writer_; }

    std::string_view view() const { return {peek(), readable_bytes()}; }

    const char *find_crlf() const;
    void append(std::string_view bytes);
    void append(const void *bytes, size_t size);
    void retrieve(size_t size);

    void retrieve_all() { reader_ = writer_ = 0; }

    std::string retrieve_as_string(size_t size);

    std::string retrieve_all_as_string() {
        return retrieve_as_string(readable_bytes());
    }

    void ensure_writable_bytes(size_t size);
    void has_written(size_t size);

  private:
    std::vector<char> bytes_;
    size_t reader_ = 0, writer_ = 0;
};
} // namespace znet
