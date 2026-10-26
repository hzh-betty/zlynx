#pragma once
#include "zco/detail/wait_queue.h"
#include <deque>
#include <mutex>
#include <stdexcept>

namespace zco {
/**
 * @brief 不可复制的有界队列，可供线程与协程发送、接收。
 * 生产者 ── send(value) ──→ [消息1][消息2] ── receive() ──→ 消费者
                              容量有限
 * @note 对象必须存活至所有操作结束；关闭后拒绝发送，但可继续取走已缓冲的值。
 */
template <class T> class Channel {
  public:
    explicit Channel(size_t capacity) : capacity_(capacity) {
        if (!capacity)
            throw std::invalid_argument("Channel capacity must be positive");
    }

    Channel(const Channel &) = delete;
    Channel &operator=(const Channel &) = delete;

    Result<void> send(T value, Deadline deadline = {}) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!closed_ && queue_.size() == capacity_) {
            auto ticket = senders_.add();
            lock.unlock();
            auto outcome = ticket.wait(deadline);
            lock.lock();
            senders_.remove(ticket);
            if (outcome != WaitOutcome::ready)
                return Result<void>(wait_error(outcome));
            if (deadline.expired(Deadline::Clock::now()) &&
                queue_.size() == capacity_)
                return Result<void>(wait_error(WaitOutcome::timeout));
        }
        if (closed_)
            return Result<void>(wait_error(WaitOutcome::closed));
        queue_.push_back(std::move(value));
        receivers_.complete_one();
        return {};
    }

    Result<T> receive(Deadline deadline = {}) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (queue_.empty() && !closed_) {
            auto ticket = receivers_.add();
            lock.unlock();
            auto outcome = ticket.wait(deadline);
            lock.lock();
            receivers_.remove(ticket);
            if (outcome != WaitOutcome::ready)
                return Result<T>(wait_error(outcome));
            if (deadline.expired(Deadline::Clock::now()) && queue_.empty())
                return Result<T>(wait_error(WaitOutcome::timeout));
        }
        if (queue_.empty())
            return Result<T>(wait_error(WaitOutcome::closed));
        T value = std::move(queue_.front());
        queue_.pop_front();
        senders_.complete_one();
        return Result<T>(std::move(value));
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        senders_.complete_all(WaitOutcome::closed);

        // 关闭后接收方仍可取走缓冲区中的值，因此唤醒后需重新检查队列。
        receivers_.complete_all();
    }

  private:
    std::mutex mutex_;
    detail::WaitQueue senders_, receivers_;
    std::deque<T> queue_;
    size_t capacity_;
    bool closed_ = false;
};
} // namespace zco
