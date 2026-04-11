#include "zco/internal/snapshot_buffer_pool.h"

namespace zco {
namespace {

// 快照按固定桶大小分配，避免共享栈切换时每次都走不可预测的大块 malloc。
// 桶越小命中越高，桶越大越容易覆盖长调用栈；超过最大桶则退化为动态分配。
constexpr size_t kSnapshotBucketSizes[] = {
    4 * 1024,  8 * 1024,   16 * 1024,  32 * 1024,
    64 * 1024, 128 * 1024, 256 * 1024, 512 * 1024,
};

} // namespace

SnapshotBufferPool::SnapshotBufferPool() : mutex_(), pools_() {}

SnapshotBufferPool::~SnapshotBufferPool() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < pools_.size(); ++i) {
        while (!pools_[i].empty()) {
            delete[] pools_[i].back();
            pools_[i].pop_back();
        }
    }
}

char *SnapshotBufferPool::acquire(size_t required_size, size_t *capacity,
                                  uint8_t *bucket_index) {
    if (required_size == 0) {
        if (capacity) {
            *capacity = 0;
        }
        if (bucket_index) {
            *bucket_index = kDynamicBucket;
        }
        return nullptr;
    }

    const uint8_t picked_bucket = pick_bucket(required_size);
    if (picked_bucket == kDynamicBucket) {
        // 超出最大桶时不再强行截断，直接按需分配，保证快照完整。
        char *buffer = new char[required_size];
        if (capacity) {
            *capacity = required_size;
        }
        if (bucket_index) {
            *bucket_index = kDynamicBucket;
        }
        return buffer;
    }

    const size_t picked_capacity = bucket_size(picked_bucket);
    char *buffer = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::deque<char *> &pool = pools_[picked_bucket];
        if (!pool.empty()) {
            // 复用已有缓冲区，减少高频保存/恢复栈快照时的分配开销。
            buffer = pool.back();
            pool.pop_back();
        }
    }

    if (!buffer) {
        buffer = new char[picked_capacity];
    }

    if (capacity) {
        *capacity = picked_capacity;
    }
    if (bucket_index) {
        *bucket_index = picked_bucket;
    }
    return buffer;
}

void SnapshotBufferPool::release(char *buffer, uint8_t bucket_index,
                                 size_t capacity) {
    if (!buffer) {
        return;
    }

    if (bucket_index == kDynamicBucket) {
        // 动态分配的快照不进入池，避免容量多样化导致池内碎片积累。
        delete[] buffer;
        return;
    }

    const size_t expected = bucket_size(bucket_index);
    if (expected == 0 || expected != capacity) {
        // 只有精确匹配的桶缓冲才允许回收，防止错误尺寸污染池子。
        delete[] buffer;
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    std::deque<char *> &pool = pools_[bucket_index];
    if (pool.size() >= kPerBucketLimit) {
        // 每桶设置上限，避免少量超大栈快照长期占住内存。
        delete[] buffer;
        return;
    }

    pool.push_back(buffer);
}

uint8_t SnapshotBufferPool::pick_bucket(size_t required_size) {
    for (uint8_t i = 0; i < kBucketCount; ++i) {
        if (required_size <= kSnapshotBucketSizes[i]) {
            // 选择“刚好够用”的最小桶，兼顾内存占用与复用率。
            return i;
        }
    }
    return kDynamicBucket;
}

size_t SnapshotBufferPool::bucket_size(uint8_t bucket_index) {
    if (bucket_index >= kBucketCount) {
        return 0;
    }
    return kSnapshotBucketSizes[bucket_index];
}

} // namespace zco