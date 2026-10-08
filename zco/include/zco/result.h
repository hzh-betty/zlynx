#pragma once
#include <new>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>

namespace zco {
enum class WaitOutcome { ready, timeout, canceled, closed };

inline std::error_code wait_error(WaitOutcome outcome) {
    switch (outcome) {
    case WaitOutcome::ready:
        return {};
    case WaitOutcome::timeout:
        return std::make_error_code(std::errc::timed_out);
    case WaitOutcome::canceled:
        return std::make_error_code(std::errc::operation_canceled);
    case WaitOutcome::closed:
        return std::make_error_code(std::errc::bad_file_descriptor);
    }
    throw std::logic_error("Invalid wait outcome");
}

template <class T> class Result {
  public:
    explicit Result(T value) : has_value_(true) {
        new (&storage_) T(std::move(value));
    }

    explicit Result(std::error_code error) : error_(error) {
        if (!error)
            throw std::invalid_argument("A failed result requires an error");
    }

    Result(const Result &) = delete;
    Result &operator=(const Result &) = delete;

    Result(Result &&other) noexcept(
        std::is_nothrow_move_constructible<T>::value)
        : error_(other.error_) {
        if (other.has_value_) {
            new (&storage_) T(std::move(*other.pointer()));
            has_value_ = true;
        }
    }

    Result &operator=(Result &&other) noexcept(
        std::is_nothrow_move_constructible<T>::value) {
        if (this != &other) {
            if (has_value_)
                pointer()->~T();
            has_value_ = false;
            error_ = other.error_;
            if (other.has_value_) {
                new (&storage_) T(std::move(*other.pointer()));
                has_value_ = true;
            }
        }
        return *this;
    }

    ~Result() {
        if (has_value_)
            pointer()->~T();
    }

    explicit operator bool() const { return has_value_ && !error_; }

    std::error_code error() const { return error_; }

    T &value() & {
        check();
        return *pointer();
    }

    const T &value() const & {
        check();
        return *pointer();
    }

    T &&value() && {
        check();
        return std::move(*pointer());
    }

  private:
    void check() const {
        if (error_)
            throw std::system_error(error_);
        if (!has_value_)
            throw std::logic_error("Result has no value");
    }

    T *pointer() { return reinterpret_cast<T *>(&storage_); }

    const T *pointer() const { return reinterpret_cast<const T *>(&storage_); }

    typename std::aligned_storage<sizeof(T), alignof(T)>::type storage_;
    bool has_value_ = false;
    std::error_code error_;
};

template <> class Result<void> {
  public:
    Result() = default;

    explicit Result(std::error_code error) : error_(error) {}

    explicit operator bool() const { return !error_; }

    std::error_code error() const { return error_; }

    void value() const {
        if (error_)
            throw std::system_error(error_);
    }

  private:
    std::error_code error_;
};
} // namespace zco
