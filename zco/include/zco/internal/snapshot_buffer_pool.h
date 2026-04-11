#ifndef ZCO_INTERNAL_SNAPSHOT_BUFFER_POOL_H_
#define ZCO_INTERNAL_SNAPSHOT_BUFFER_POOL_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

#include "zco/internal/noncopyable.h"

namespace zco {

class SnapshotBufferPool : public NonCopyable {
  public:
    SnapshotBufferPool();
    ~SnapshotBufferPool();

    /**
     * @brief 获取一个满足要求的缓冲区
     * @param required_size 请求的最小缓冲区大小
     * @param capacity 输出实际分配的缓冲区容量
     * @param bucket_index 输出分配使用的桶索引，调用方应传回 release()
     * 以便归还正确的池
     * @return 分配的缓冲区指针，调用方使用完毕后必须调用 release() 归还
     */
    char *acquire(size_t required_size, size_t *capacity,
                  uint8_t *bucket_index);
    /**
     * @brief 归还缓冲区
     * @param buffer 待归还的缓冲区指针
     * @param bucket_index 缓冲区所属的桶索引
     * @param capacity 缓冲区容量
     */

    void release(char *buffer, uint8_t bucket_index, size_t capacity);

    static constexpr uint8_t dynamic_bucket_index() { return kDynamicBucket; }

  private:
    static constexpr size_t kBucketCount = 8;
    static constexpr uint8_t kDynamicBucket = 0xff;
    static constexpr size_t kPerBucketLimit = 256;

    /**
     * @brief 选择合适的桶
     * @param required_size 请求的最小缓冲区大小
     * @return 桶索引
     */
    static uint8_t pick_bucket(size_t required_size);

    /**
     * @brief 获取指定桶的大小
     * @param bucket_index 桶索引
     * @return 桶的大小
     */
    static size_t bucket_size(uint8_t bucket_index);

    std::mutex mutex_;
    std::array<std::deque<char *>, kBucketCount> pools_;
};

} // namespace zco

#endif // ZCO_INTERNAL_SNAPSHOT_BUFFER_POOL_H_