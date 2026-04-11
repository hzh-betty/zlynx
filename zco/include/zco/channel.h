#ifndef ZCO_CHANNEL_H_
#define ZCO_CHANNEL_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

#include "zco/event.h"
#include "zco/zco_log.h"

namespace zco {

/**
 * @brief 协程与线程可共享的有界通道。
 * @tparam T 元素类型。
 */
template <typename T> class Channel {
  public:
    /**
     * @brief 构造通道。
     * @param capacity 缓冲容量，最小为 1。
     * @param timeout_ms 默认读写超时。
     */
    explicit Channel(uint32_t capacity = 1,
                     uint32_t timeout_ms = kInfiniteTimeoutMs)
        : capacity_(capacity == 0 ? 1 : capacity), timeout_ms_(timeout_ms),
          storage_(capacity_, StorageSlot{}), head_(0), tail_(0), size_(0),
          not_empty_(true, false), not_full_(true, true), closed_(false),
          done_(true) {}

    ~Channel() {
        std::lock_guard<std::mutex> lock(mutex_);
        clear_locked();
    }

    /**
     * @brief 从通道读取一个元素。
     * @param out 输出对象。
     * @return true 表示读取成功，false 表示超时或通道关闭且为空。
     */
    bool read(T &out) const { return read(out, timeout_ms_); }

    /**
     * @brief 从通道读取一个元素（按调用覆盖超时）。
     * @param out 输出对象。
     * @param timeout_ms 本次读取超时。
     * @return true 表示读取成功。
     */
    bool read(T &out, uint32_t timeout_ms) const {
        while (true) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (size_ > 0) {
                    pop_front_locked(&out);
                    done_.store(true, std::memory_order_release);
                    update_events_locked();
                    ZCO_LOG_DEBUG("channel read success, size={}, capacity={}",
                                  size_, capacity_);
                    return true;
                }

                const bool closed = closed_.load(std::memory_order_acquire);
                if (closed) {
                    done_.store(false, std::memory_order_release);
                    update_events_locked();
                    ZCO_LOG_DEBUG(
                        "channel read failed, channel closed and empty");
                    return false;
                }
            }

            if (!not_empty_.wait(timeout_ms)) {
                done_.store(false, std::memory_order_release);
                ZCO_LOG_DEBUG("channel read timeout, timeout_ms={}",
                              timeout_ms);
                return false;
            }
        }
    }

    /**
     * @brief 向通道写入一个元素（拷贝）。
     * @param value 输入元素。
     * @return true 表示写入成功。
     */
    bool write(const T &value) const {
        T copy = value;
        return write_impl(std::move(copy), timeout_ms_);
    }

    /**
     * @brief 向通道写入一个元素（拷贝，按调用覆盖超时）。
     * @param value 输入元素。
     * @param timeout_ms 本次写入超时。
     * @return true 表示写入成功。
     */
    bool write(const T &value, uint32_t timeout_ms) const {
        T copy = value;
        return write_impl(std::move(copy), timeout_ms);
    }

    /**
     * @brief 向通道写入一个元素（移动）。
     * @param value 输入元素。
     * @return true 表示写入成功。
     */
    bool write(T &&value) const {
        return write_impl(std::move(value), timeout_ms_);
    }

    /**
     * @brief 向通道写入一个元素（移动，按调用覆盖超时）。
     * @param value 输入元素。
     * @param timeout_ms 本次写入超时。
     * @return true 表示写入成功。
     */
    bool write(T &&value, uint32_t timeout_ms) const {
        return write_impl(std::move(value), timeout_ms);
    }

    /**
     * @brief 尝试立即读取（不等待）。
     * @param out 输出对象。
     * @return true 表示读取成功。
     */
    bool try_read(T &out) const { return read(out, 0); }

    /**
     * @brief 尝试立即写入（不等待）。
     * @param value 输入元素。
     * @return true 表示写入成功。
     */
    bool try_write(const T &value) const { return write(value, 0); }

    /**
     * @brief 尝试立即写入（不等待）。
     * @param value 输入元素。
     * @return true 表示写入成功。
     */
    bool try_write(T &&value) const { return write(std::move(value), 0); }

    /**
     * @brief 管道读取操作符。
     * @param out 输出对象。
     * @return 当前通道对象。
     */
    Channel &operator>>(T &out) {
        (void)read(out);
        return *this;
    }

    /**
     * @brief 管道写入操作符（拷贝）。
     * @param value 输入元素。
     * @return 当前通道对象。
     */
    Channel &operator<<(const T &value) {
        (void)write(value);
        return *this;
    }

    /**
     * @brief 管道写入操作符（移动）。
     * @param value 输入元素。
     * @return 当前通道对象。
     */
    Channel &operator<<(T &&value) {
        (void)write(std::move(value));
        return *this;
    }

    /**
     * @brief 返回最近一次读写是否成功。
     * @return true 表示最近一次操作成功。
     */
    bool done() const { return done_.load(std::memory_order_acquire); }

    /**
     * @brief 关闭通道。
     * @return 无返回值。
     */
    void close() const {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_.store(true, std::memory_order_release);
            update_events_locked();
            ZCO_LOG_INFO("channel closed, remaining_size={}, capacity={}",
                         size_, capacity_);
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    /**
     * @brief 判断通道是否仍可写。
     * @return true 表示未关闭。
     */
    explicit operator bool() const {
        return !closed_.load(std::memory_order_acquire);
    }

  private:
    /**
     * @brief 写入实现。
     * @param value 输入元素。
     * @return true 表示写入成功。
     */
    bool write_impl(T &&value, uint32_t timeout_ms) const {
        while (true) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const bool closed = closed_.load(std::memory_order_acquire);
                if (closed) {
                    done_.store(false, std::memory_order_release);
                    update_events_locked();
                    ZCO_LOG_WARN(
                        "channel write failed, channel already closed");
                    return false;
                }

                if (size_ < capacity_) {
                    push_back_locked(std::move(value));
                    done_.store(true, std::memory_order_release);
                    update_events_locked();
                    ZCO_LOG_DEBUG("channel write success, size={}, capacity={}",
                                  size_, capacity_);
                    return true;
                }
            }

            if (!not_full_.wait(timeout_ms)) {
                done_.store(false, std::memory_order_release);
                ZCO_LOG_DEBUG("channel write timeout, timeout_ms={}",
                              timeout_ms);
                return false;
            }
        }
    }

    /**
     * @brief 在持锁状态下更新内部事件状态。
     * @return 无返回值。
     */
    void update_events_locked() const {
        // not_empty_ 和 not_full_ 分别用于读侧和写侧阻塞唤醒。
        if (size_ > 0) {
            not_empty_.signal();
        } else {
            not_empty_.reset();
        }

        const bool closed = closed_.load(std::memory_order_acquire);
        const bool can_write = !closed && size_ < capacity_;
        if (can_write) {
            not_full_.signal();
        } else {
            not_full_.reset();
        }
    }

    using StorageSlot =
        typename std::aligned_storage<sizeof(T), alignof(T)>::type;

    T *slot_ptr(size_t index) const {
        return reinterpret_cast<T *>(
            const_cast<StorageSlot *>(&storage_[index]));
    }

    void push_back_locked(T &&value) const {
        new (slot_ptr(tail_)) T(std::move(value));
        tail_ = (tail_ + 1U) % capacity_;
        ++size_;
    }

    void pop_front_locked(T *out) const {
        T *element = slot_ptr(head_);
        *out = std::move(*element);
        element->~T();
        head_ = (head_ + 1U) % capacity_;
        --size_;
    }

    void clear_locked() const {
        while (size_ > 0) {
            T *element = slot_ptr(head_);
            element->~T();
            head_ = (head_ + 1U) % capacity_;
            --size_;
        }
        head_ = 0;
        tail_ = 0;
    }

    const size_t capacity_;
    const uint32_t timeout_ms_;
    mutable std::mutex mutex_;
    mutable std::vector<StorageSlot> storage_; // 固定容量的环形缓冲区
    mutable size_t head_;
    mutable size_t tail_;
    mutable size_t size_;
    mutable Event not_empty_;
    mutable Event not_full_;
    mutable std::atomic<bool> closed_;
    mutable std::atomic<bool> done_;
};

} // namespace zco

#endif // ZCO_CHANNEL_H_
