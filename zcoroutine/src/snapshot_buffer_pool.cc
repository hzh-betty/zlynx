#include "zcoroutine/internal/snapshot_buffer_pool.h"

namespace zcoroutine {
namespace {

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
        delete[] buffer;
        return;
    }

    const size_t expected = bucket_size(bucket_index);
    if (expected == 0 || expected != capacity) {
        delete[] buffer;
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    std::deque<char *> &pool = pools_[bucket_index];
    if (pool.size() >= kPerBucketLimit) {
        delete[] buffer;
        return;
    }

    pool.push_back(buffer);
}

uint8_t SnapshotBufferPool::pick_bucket(size_t required_size) {
    for (uint8_t i = 0; i < kBucketCount; ++i) {
        if (required_size <= kSnapshotBucketSizes[i]) {
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

} // namespace zcoroutine