#include "zco/pool.h"

#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "zco/zco_log.h"

namespace zco {

// Pool 的实现策略：
// - 以线程 ID 为 key 维护分桶，降低跨线程竞争。
// - 每个桶使用 vector<void*> 作为 LIFO 缓存，提升复用局部性。
// - 超出容量时走 destroy 回调，避免无限膨胀。
// - capacity == static_cast<size_t>(-1) 表示不设上限。

struct Pool::Impl {
    /**
     * @brief 构造对象池实现。
     * @param create_cb_arg 创建回调。
     * @param destroy_cb_arg 销毁回调。
     * @param capacity_arg 容量上限。
     */
    Impl(CreateCallback create_cb_arg, DestroyCallback destroy_cb_arg,
         size_t capacity_arg)
        : create_cb(std::move(create_cb_arg)),
          destroy_cb(std::move(destroy_cb_arg)), capacity(capacity_arg),
          mutex(), buckets() {}

    /**
     * @brief 析构时清理全部对象。
     */
    ~Impl() { clear_all(); }

    /**
     * @brief 弹出对象。
     * @return 对象指针。
     */
    void *pop() {
        // 线程本地桶优先复用，减少对象构造与析构成本。
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<void *> &bucket = buckets[std::this_thread::get_id()];
        if (!bucket.empty()) {
            void *element = bucket.back();
            bucket.pop_back();
            ZCO_LOG_DEBUG("pool pop from bucket, remaining_size={}",
                                 bucket.size());
            return element;
        }

        if (create_cb) {
            ZCO_LOG_DEBUG("pool pop triggers create callback");
            return create_cb();
        }
        ZCO_LOG_DEBUG(
            "pool pop returns null, no cached element and no create callback");
        return nullptr;
    }

    /**
     * @brief 归还对象。
     * @param element 对象指针。
     * @return 无返回值。
     */
    void push(void *element) {
        if (!element) {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex);
        std::vector<void *> &bucket = buckets[std::this_thread::get_id()];
        if (destroy_cb && capacity != static_cast<size_t>(-1) &&
            bucket.size() >= capacity) {
            // 容量受限时即时回收，避免桶无限增长。
            ZCO_LOG_DEBUG("pool push triggers destroy callback, "
                                 "bucket_size={}, capacity={}",
                                 bucket.size(), capacity);
            destroy_cb(element);
            return;
        }
        bucket.push_back(element);
        ZCO_LOG_DEBUG("pool push success, bucket_size={}",
                             bucket.size());
    }

    /**
     * @brief 查询当前线程桶大小。
     * @return 元素数量。
     */
    size_t size() const {
        // size 查询的是“当前线程桶”大小，而非全局总和。
        std::lock_guard<std::mutex> lock(mutex);
        auto it = buckets.find(std::this_thread::get_id());
        if (it == buckets.end()) {
            return 0;
        }
        return it->second.size();
    }

    /**
     * @brief 清空所有线程桶。
     * @return 无返回值。
     */
    void clear_all() {
        // clear_all 是全局清理动作，遍历所有线程桶。
        std::lock_guard<std::mutex> lock(mutex);
        size_t release_count = 0;
        for (auto it = buckets.begin(); it != buckets.end(); ++it) {
            std::vector<void *> &bucket = it->second;
            while (!bucket.empty()) {
                void *element = bucket.back();
                bucket.pop_back();
                if (destroy_cb) {
                    destroy_cb(element);
                }
                ++release_count;
            }
        }
        buckets.clear();
        ZCO_LOG_INFO("pool clear all finished, released_count={}",
                            release_count);
    }

    CreateCallback create_cb;
    DestroyCallback destroy_cb;
    size_t capacity;
    mutable std::mutex mutex;
    std::unordered_map<std::thread::id, std::vector<void *>> buckets;
};

Pool::Pool(CreateCallback create_cb, DestroyCallback destroy_cb,
           size_t capacity)
    : impl_(std::make_shared<Impl>(std::move(create_cb), std::move(destroy_cb),
                                   capacity)) {
    ZCO_LOG_INFO("pool created, capacity={}", capacity);
}

Pool::~Pool() = default;

Pool::Pool(const Pool &other) = default;

Pool::Pool(Pool &&other) noexcept = default;

void *Pool::pop() const {
    // 空 impl 通常只会出现在移动后对象或异常构造路径。
    if (!impl_) {
        return nullptr;
    }
    return impl_->pop();
}

void Pool::push(void *element) const {
    if (!impl_) {
        return;
    }
    impl_->push(element);
}

size_t Pool::size() const {
    if (!impl_) {
        return 0;
    }
    return impl_->size();
}

void Pool::clear() const {
    if (!impl_) {
        return;
    }
    impl_->clear_all();
}

} // namespace zco
