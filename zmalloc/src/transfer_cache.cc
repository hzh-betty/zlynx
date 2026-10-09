/**
 * @file transfer_cache.cc
 * @brief TransferCache 实现
 * @author hzh-betty
 */

#include "zmalloc/internal/transfer_cache.h"

#include <algorithm>
#include <mutex>

#include "zmalloc/internal/central_cache.h"
#include "zmalloc/internal/free_list.h"

#include "zmalloc/internal/size_class.h"

namespace zmalloc {

constexpr size_t TransferCacheEntry::kMaxCacheSlots;

TransferCache::TransferCache() {
    // 每规格最多保留 64KiB（大对象至少两个），避免 2048 个大对象常驻。
    for (size_t size = 1; size <= MAX_BYTES;) {
        const auto &e = SizeClass::lookup(size);
        entries_[e.index].capacity_ =
            std::min<size_t>(TransferCacheEntry::kMaxCacheSlots,
                             std::max<size_t>(
                                 2, TRANSFER_CACHE_BUDGET / e.align_size));
        size = e.align_size + 1;
    }
}

size_t TransferCacheEntry::insert_range(void *batch[], size_t count) {
    if (count == 0) {
        return 0;
    }

    // 快路径：锁外快速判断是否已满，尽量减少锁竞争。
    const size_t cur = count_.load(std::memory_order_relaxed);
    if (cur >= capacity_) {
        return 0;
    }

    // 短临界区：搬运指针 + 更新 head/count
    std::lock_guard<SpinLock> lock(mtx_);

    // 计算可插入的数量
    const size_t cur_locked = count_.load(std::memory_order_relaxed);
    const size_t available = capacity_ - cur_locked;
    const size_t to_insert = std::min(count, available);

    // 二次确认：锁内重新计算，确保并发下正确。
    if (to_insert == 0) { // GCOVR_EXCL_LINE
        return 0;
    }

    // head_ 可能在数组中间，拆成两段复制。
    const size_t first = std::min(to_insert, kMaxCacheSlots - head_);
    std::copy_n(batch, first, slots_.data() + head_);
    if (to_insert > first) {
        std::copy_n(batch + first, to_insert - first, slots_.data());
    }

    head_ = (head_ + to_insert) & kMask;
    count_.store(cur_locked + to_insert, std::memory_order_relaxed);

    return to_insert;
}

size_t TransferCacheEntry::remove_range(void *batch[], size_t count) {
    if (count == 0) {
        return 0;
    }

    // 快路径：锁外快速判断是否为空，尽量减少锁竞争。
    if (count_.load(std::memory_order_relaxed) == 0) {
        return 0;
    }

    std::lock_guard<SpinLock> lock(mtx_);

    // 计算可取出的数量
    const size_t cur_locked = count_.load(std::memory_order_relaxed);
    const size_t to_remove = std::min(count, cur_locked);

    // 二次确认：锁内重新计算，确保并发下正确。
    if (to_remove == 0) { // GCOVR_EXCL_LINE
        return 0;
    }

    const size_t first = std::min(to_remove, kMaxCacheSlots - tail_);
    std::copy_n(slots_.data() + tail_, first, batch);
    if (to_remove > first) {
        std::copy_n(slots_.data(), to_remove - first, batch + first);
    }

    tail_ = (tail_ + to_remove) & kMask;
    count_.store(cur_locked - to_remove, std::memory_order_relaxed);

    return to_remove;
}

size_t TransferCacheEntry::size() const {
    return count_.load(std::memory_order_relaxed);
}

size_t TransferCache::insert_range(size_t index, void *batch[], size_t count) {
    return entries_[index].insert_range(batch, count);
}

size_t TransferCache::remove_range(size_t index, void *batch[], size_t count) {
    return entries_[index].remove_range(batch, count);
}

size_t TransferCache::drain() {
    size_t bytes = 0;
    std::array<void *, MAX_BATCH_SIZE> objects;
    for (size_t size = 1; size <= MAX_BYTES;) {
        const auto &e = SizeClass::lookup(size);
        // 只处理初始数量的上限；并发补货不会让回收无限追赶生产者。
        size_t remaining = entries_[e.index].size();
        while (remaining != 0) {
            const size_t removed = remove_range(
                e.index, objects.data(), std::min(remaining, objects.size()));
            if (removed == 0) {
                break;
            }
            // remove_range 已释放传输锁，再归还中心层，避免形成嵌套锁。
            for (size_t i = 1; i < removed; ++i) {
                next_obj(objects[i - 1]) = objects[i];
            }
            next_obj(objects[removed - 1]) = nullptr;
            CentralCache::get_instance().release_list_to_spans(objects[0], e.index);
            bytes += removed * e.align_size;
            remaining -= removed;
        }
        size = e.align_size + 1;
    }
    return bytes;
}

size_t TransferCache::cached_bytes() const {
    size_t bytes = 0;
    for (size_t size = 1; size <= MAX_BYTES;) {
        const auto &e = SizeClass::lookup(size);
        bytes += entries_[e.index].size() * e.align_size;
        size = e.align_size + 1;
    }
    return bytes;
}

} // namespace zmalloc
