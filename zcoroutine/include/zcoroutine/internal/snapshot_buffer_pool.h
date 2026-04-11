#ifndef ZCOROUTINE_INTERNAL_SNAPSHOT_BUFFER_POOL_H_
#define ZCOROUTINE_INTERNAL_SNAPSHOT_BUFFER_POOL_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

#include "zcoroutine/internal/noncopyable.h"

namespace zcoroutine {

class SnapshotBufferPool : public NonCopyable {
  public:
    SnapshotBufferPool();
    ~SnapshotBufferPool();

    char *acquire(size_t required_size, size_t *capacity,
                  uint8_t *bucket_index);

    void release(char *buffer, uint8_t bucket_index, size_t capacity);

    static constexpr uint8_t dynamic_bucket_index() { return kDynamicBucket; }

  private:
    static constexpr size_t kBucketCount = 8;
    static constexpr uint8_t kDynamicBucket = 0xff;
    static constexpr size_t kPerBucketLimit = 256;

    static uint8_t pick_bucket(size_t required_size);
    static size_t bucket_size(uint8_t bucket_index);

    std::mutex mutex_;
    std::array<std::deque<char *>, kBucketCount> pools_;
};

} // namespace zcoroutine

#endif // ZCOROUTINE_INTERNAL_SNAPSHOT_BUFFER_POOL_H_