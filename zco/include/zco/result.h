#ifndef ZLYNX_ZCO_INCLUDE_ZCO_RESULT_H_
#define ZLYNX_ZCO_INCLUDE_ZCO_RESULT_H_

#include <new>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>

namespace zco {
/**
 * @brief 一次等待的终态；ready 不代表后续 I/O 已完成或必然成功。
 */
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

/**
 * @brief 持有值或错误码的操作结果，不支持复制。
 *
 * 构造失败结果时错误码必须非零。移动不会清空源对象的成功标记，
 * 源对象中的值处于 T 自身的移动后状态；不能据此推断资源仍然可用。
 *
 * @tparam T 可移动构造、可析构的结果类型。
 */
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

    /**
     * @brief 访问成功值；三个重载分别返回可写、只读或可移动的引用。
     *
     * @return 借用内部值；结果销毁或赋值后不得继续使用原引用。
     * @throws std::system_error 结果包含错误码。
     * @throws std::logic_error 结果不持有值。
     */
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

    typename std::aligned_storage<sizeof(T), alignof(T)>::type storage_; // 存储值的未初始化缓冲区
    bool has_value_ = false;
    std::error_code error_;
};

/**
 * @brief 无返回值操作的状态；默认构造或零错误码表示成功。
 *
 * value() 在失败时抛出 std::system_error，成功时无操作。
 */
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

#endif
