#ifndef ZLOG_BUFFER_H_
#define ZLOG_BUFFER_H_
#include <cstddef>

namespace zlog {
// 缓冲区大小常量定义
static constexpr size_t DEFAULT_BUFFER_SIZE =
    1024 * 1024 * 2; // 默认缓冲区大小：2MB
static constexpr size_t THRESHOLD_BUFFER_SIZE =
    1024 * 1024 * 8; // 阈值缓冲区大小：8MB
static constexpr size_t INCREMENT_BUFFER_SIZE =
    1024 * 1024 * 1; // 增量缓冲区大小：1MB
static constexpr size_t MAX_BUFFER_SIZE =
    1024 * 1024 * 512; // 最大缓冲区大小：512MB

/**
 * @brief 双缓冲区类
 * 用于异步日志系统的缓冲区管理，支持动态扩容
 * 之所以不用 std::vector 是因为频繁的构造与析构调用__memset_avx2_unaligned_erms
 */
class alignas(64) Buffer {
public:
  /**
   * @brief 构造函数
   * 初始化缓冲区为默认大小
   */
  Buffer();

  /**
   * @brief 析构函数
   * 释放缓冲区内存
   */
  ~Buffer();

  /**
   * @brief 向缓冲区写入数据
   * @param data 数据指针
   * @param len 数据长度
   */
  void push(const char *data, size_t len);

  /**
   * @brief 返回可读数据的起始位置
   * @return 可读数据的指针
   */
  const char *begin() const;

  /**
   * @brief 返回可写数据的长度
   * @return 可写空间大小
   */
  size_t writeAbleSize() const;

  /**
   * @brief 返回可读数据的长度
   * @return 可读数据大小
   */
  size_t readAbleSize() const;

  /**
   * @brief 移动读指针
   * @param len 要移动的长度
   */
  void moveReader(size_t len);

  /**
   * @brief 重置读写位置，初始化缓冲区
   */
  void reset();

  /**
   * @brief 与另一个缓冲区交换内容
   * @param buffer 要交换的缓冲区
   */
  void swap(Buffer &buffer) noexcept;

  /**
   * @brief 判断缓冲区是否为空
   * @return 空返回true，否则返回false
   */
  bool empty() const;

  /**
   * @brief 检查是否可以容纳指定长度的数据而不超过最大缓冲区大小
   * @param len 需要的空间大小
   * @return 可以容纳返回true，否则返回false
   */
  bool canAccommodate(size_t len) const;

  /**
   * @brief 获取缓冲区容量
   * @return 缓冲区总大小
   */
  size_t capacity() const { return capacity_; }

private:
  /**
   * @brief 确保缓冲区有足够空间
   * @param len 需要的空间大小
   */
  void ensureEnoughSize(size_t len);

  /**
   * @brief 计算扩容后的新缓冲区大小
   * @param len 需要额外容纳的空间大小
   * @return 计算得到的新缓冲区大小
   */
  size_t calculateNewSize(size_t len) const;

  /**
   * @brief 移动写指针
   * @param len 要移动的长度
   */
  void moveWriter(size_t len);

private:
  char *data_;       // 缓冲区指针
  size_t writerIdx_; // 当前可写数据的下标 (热路径)
  size_t capacity_;  // 缓冲区总容量
  size_t readerIdx_; // 当前可读数据的下标
};
} // namespace zlog

#endif // ZLOG_BUFFER_H_
