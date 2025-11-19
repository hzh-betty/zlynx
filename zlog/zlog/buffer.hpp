#pragma once
#include <cassert>
#include <vector>
#include <algorithm>

namespace zlog
{
    // 缓冲区大小常量定义
    static constexpr size_t DEFAULT_BUFFER_SIZE = 1024 * 1024 * 2;      ///< 默认缓冲区大小：2MB
    static constexpr size_t THRESHOLD_BUFFER_SIZE = 1024 * 1024 * 8;    ///< 阈值缓冲区大小：8MB
    static constexpr size_t INCREMENT_BUFFER_SIZE = 1024 * 1024 * 1;    ///< 增量缓冲区大小：1MB

    /**
     * @brief 双缓冲区类
     * 用于异步日志系统的缓冲区管理，支持动态扩容
     */
    class Buffer
    {
    public:
        /**
         * @brief 构造函数
         * 初始化缓冲区为默认大小
         */
        Buffer()
            : buffer_(DEFAULT_BUFFER_SIZE), writerIdx_(0), readerIdx_(0)
        {
        }

        /**
         * @brief 向缓冲区写入数据
         * @param data 数据指针
         * @param len 数据长度
         */
        void push(const char *data, size_t len)
        {
            // 1.动态扩容
            ensureEnoughSize(len); // 动态空间增长，用于极限测试

            // 2.拷贝进缓冲区
            std::copy_n(data, len, &buffer_[writerIdx_]);

            // 3.将当前位置往后移动
            moveWriter(len);
        }

        /**
         * @brief 返回可读数据的起始位置
         * @return 可读数据的指针
         */
        const char *begin() const
        {
            return &buffer_[readerIdx_];
        }

        /**
         * @brief 返回可写数据的长度
         * @return 可写空间大小
         */
        size_t writeAbleSize() const
        {
            // 为固定大小缓冲区提供!!!
            return (buffer_.size() - writerIdx_);
        }

        /**
         * @brief 返回可读数据的长度
         * @return 可读数据大小
         */
        size_t readAbleSize() const
        {
            return writerIdx_ - readerIdx_;
        }

        /**
         * @brief 移动读指针
         * @param len 要移动的长度
         */
        void moveReader(const size_t len)
        {
            assert(len <= readAbleSize());
            readerIdx_ += len;
        }

        /**
         * @brief 重置读写位置，初始化缓冲区
         */
        void reset()
        {
            readerIdx_ = writerIdx_ = 0;
        }

        /**
         * @brief 与另一个缓冲区交换内容
         * @param buffer 要交换的缓冲区
         */
        void swap(Buffer &buffer) noexcept
        {
            buffer_.swap(buffer.buffer_);
            std::swap(readerIdx_, buffer.readerIdx_);
            std::swap(writerIdx_, buffer.writerIdx_);
        }

        /**
         * @brief 判断缓冲区是否为空
         * @return 空返回true，否则返回false
         */
        bool empty() const
        {
            return readerIdx_ == writerIdx_;
        }

    private:
        /**
         * @brief 确保缓冲区有足够空间
         * @param len 需要的空间大小
         */
        void ensureEnoughSize(const size_t len)
        {
            if (len <= writeAbleSize())
                return;
            size_t newSize = 0;
            // 1. 小于阈值翻倍增长
            if (buffer_.size() < THRESHOLD_BUFFER_SIZE)
            {
                newSize = buffer_.size() * 2 + len;
            }
            // 大于阈值增量增长
            else
            {
                newSize = buffer_.size() + INCREMENT_BUFFER_SIZE + len;
            }

            buffer_.resize(newSize);
        }
        
        /**
         * @brief 移动写指针
         * @param len 要移动的长度
         */
        void moveWriter(const size_t len)
        {
            assert(len <= writeAbleSize());
            writerIdx_ += len;
        }

    private:
        std::vector<char> buffer_;      ///< 缓冲区
        size_t writerIdx_;              ///< 当前可写数据的下标
        size_t readerIdx_;              ///< 当前可读数据的下标
    };
} // namespace zlog