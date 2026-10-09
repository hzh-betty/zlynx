#pragma once

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

namespace znet {

enum class ErrorKind { validation, runtime, io, protocol, application };

struct Error {
    ErrorKind kind = ErrorKind::io;
    std::error_code code;
    std::string operation;
    std::string detail;

    explicit operator bool() const { return bool(code); }

    std::string message() const;
};

Error make_error(ErrorKind kind, std::errc code, std::string operation,
                 std::string detail = {});
Error io_error(std::string operation, int code);
Error runtime_error(std::string operation, std::error_code code);

// Runtime failures are values; accessing the wrong alternative is a contract
// violation. No errno or optional output arguments form part of this API.
template <class T> class [[nodiscard]] Result {
  public:
    Result(T value) : storage_(std::move(value)) {}

    Result(Error error) : storage_(std::move(error)) {
        if (!std::get<Error>(storage_))
            throw std::invalid_argument("A failed result needs an error");
    }

    explicit operator bool() const {
        return std::holds_alternative<T>(storage_);
    }

    T &value() & { return std::get<T>(storage_); }

    const T &value() const & { return std::get<T>(storage_); }

    T &&value() && { return std::get<T>(std::move(storage_)); }

    const Error &error() const { return std::get<Error>(storage_); }

  private:
    std::variant<T, Error> storage_;
};

template <> class [[nodiscard]] Result<void> {
  public:
    Result() = default;

    Result(Error error) : error_(std::move(error)) {
        if (!*error_)
            throw std::invalid_argument("A failed result needs an error");
    }

    explicit operator bool() const { return !error_; }

    void value() const {
        if (error_)
            throw std::logic_error("Accessing a failed result");
    }

    const Error &error() const {
        if (!error_)
            throw std::logic_error("Accessing a successful result's error");
        return *error_;
    }

  private:
    std::optional<Error> error_;
};

// bytes remains meaningful on failure. eof is reserved for stream EOF, never
// a zero-length datagram or a zero-length request.
struct [[nodiscard]] Transfer {
    size_t bytes = 0;
    Error error;
    bool eof = false;

    explicit operator bool() const { return !error; }
};
} // namespace znet
