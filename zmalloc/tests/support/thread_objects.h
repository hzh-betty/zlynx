#ifndef ZLYNX_ZMALLOC_TESTS_SUPPORT_THREAD_OBJECTS_H_
#define ZLYNX_ZMALLOC_TESTS_SUPPORT_THREAD_OBJECTS_H_

#include "zmalloc/internal/thread_cache.h"
#include <unordered_map>

namespace zmalloc::test {
class ThreadObjects {
  public:
    explicit ThreadObjects(ThreadCache &cache) : cache_(cache) {}

    ~ThreadObjects() {
        clear();
        cache_.cleanup();
    }

    void *allocate(size_t size) {
        void *object = cache_.allocate(size);
        try {
            live_.emplace(object, size);
        } catch (...) {
            cache_.deallocate(object, size);
            throw;
        }
        return object;
    }

    void release(void *object) { live_.erase(object); }

    void adopt(void *object, size_t size) {
        try {
            live_.emplace(object, size);
        } catch (...) {
            cache_.deallocate(object, size);
            throw;
        }
    }

    void deallocate(void *object, size_t size) {
        live_.erase(object);
        cache_.deallocate(object, size);
    }

    void clear() {
        for (auto object : live_)
            cache_.deallocate(object.first, object.second);
        live_.clear();
    }

  private:
    ThreadCache &cache_;
    std::unordered_map<void *, size_t> live_;
};
} // namespace zmalloc::test

#endif
