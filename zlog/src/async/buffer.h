/**
 * @file buffer.h
 * @author hzh-betty
 */
#ifndef ZLOG_INTERNAL_BUFFER_H_
#define ZLOG_INTERNAL_BUFFER_H_
#include <cstddef>

namespace zlog {
namespace detail {
static constexpr size_t kDefaultBufferSize =
    1024 * 1024 * 2; // 默认缓冲区大小：2MB
static constexpr size_t kThresholdBufferSize =
    1024 * 1024 * 8; // 阈值缓冲区大小：8MB
static constexpr size_t kMaxBufferSize =
    1024 * 1024 * 512; // 最大缓冲区大小：512MB

/**
 * @brief 独占连续字节存储的追加缓冲区，为异步日志批量交换提供基础。
 *
 * 只跟踪已写入长度，不解析记录；双缓冲关系由 AsyncLooper 管理。
 * 本类不加锁，扩容上限为 kMaxBufferSize；重置保留已分配容量。
 */
class alignas(64) Buffer {
  public:
    Buffer();

    // 缓冲区独占底层内存，禁止浅拷贝；交换通过 swap 显式完成。
    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;

    ~Buffer();

    /**
     * @brief 复制并追加原始字节，保留内嵌 NUL；必要时扩容。
     *
     * @param data 输入存储；len 为 0 时允许为空且不做操作。
     * @param len 待追加字节数。
     * @pre 非零长度时，data 指向至少 len 个可读字节，不与待写区域重叠；
     * 可能扩容时不得借用本缓冲区的存储。
     * @throws std::invalid_argument 非零长度时 data 为空。
     * @throws std::length_error 追加后超过容量上限。
     * @throws std::bad_alloc 扩容分配失败。
     */
    void push(const char *data, size_t len);

    /**
     * @brief 为额外 len 个字节预留空间，不改变已写入长度。
     *
     * 失败时保留已有数据；成功扩容可能使此前借用的指针失效。
     * @throws std::length_error 所需空间超过容量上限。
     * @throws std::bad_alloc 扩容分配失败。
     */
    void reserve(size_t len);

    /**
     * @brief 借用已写入数据的起始地址，可读长度为 readable_size()。
     *
     * 扩容或销毁后指针失效；swap 后存储归另一缓冲区所有，reset
     * 后原内容不再属于可读范围。
     */
    const char *begin() const;

    size_t writable_size() const;

    size_t readable_size() const;

    /**
     * @brief 清空可读范围但保留存储，不覆盖原字节。
     */
    void reset();

    /**
     * @brief 交换存储所有权、已写入长度和容量，不复制底层字节。
     */
    void swap(Buffer &buffer) noexcept;

    bool empty() const;

    /**
     * @brief 判断追加 len 个字节是否仍在容量上限内，不执行分配。
     */
    bool can_accommodate(size_t len) const;

    size_t capacity() const { return capacity_; }

  private:
    /**
     * @brief 确保缓冲区有足够空间
     * @param len 需要的空间大小
     */
    void ensure_enough_size(size_t len);

    /**
     * @brief 计算扩容后的新缓冲区大小
     * @param len 需要额外容纳的空间大小
     * @return 计算得到的新缓冲区大小
     */
    size_t calculate_new_size(size_t len) const;

  private:
    // [0, writer_idx_) 为有效字节，剩余容量尚未写入；所有者负责串行访问。
    char *data_;
    size_t writer_idx_;
    size_t capacity_;
};
} // namespace detail
} // namespace zlog

#endif
