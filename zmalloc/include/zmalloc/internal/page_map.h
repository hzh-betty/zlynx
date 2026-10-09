/**
 * @file page_map.h
 * @brief 基数树实现，用于页号到 Span 的高效映射
 *
 * @author hzh-betty
 *
 * X86 (32位): 二层基数树 PageMap2
 * X64 (64位):
 * 三层基数树 PageMap3
 */

#ifndef ZMALLOC_INTERNAL_PAGE_MAP_H_
#define ZMALLOC_INTERNAL_PAGE_MAP_H_

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>

#include "object_pool.h"
#include "system_alloc.h"
#include "zmalloc_config.h"

namespace zmalloc {

// 各层 PageMap 均允许单写者与多个查询者并发访问；写操作由调用方串行化。
// 节点完整初始化后以 release 发布，查询以 acquire 读取；发布后的节点不回收。
// 叶项同样使用原子发布，但返回指针不延长目标对象的生命周期。查询者解引用前
// 必须持有对象的存活保证或调用方的外部锁；销毁 PageMap 前须停止所有访问。

/**
 * @brief 一层基数树（适用于小地址空间/小 BITS）
 *
 * 这是最简单的页号 -> 指针映射：直接用数组下标访问。
 *
 * 注意：空间开销为 O(2^BITS)。为了避免误用导致超大内存占用，
 * 这里用 static_assert 限制 BITS 不能太大（用于单元测试与小场景）。
 */
template <int BITS> class PageMap1 {
  public:
    using Number = uintptr_t;
    static_assert(BITS > 0, "BITS must be positive");
    static_assert(BITS <= 20,
                  "PageMap1 is only intended for small BITS (<=20)");

    static constexpr size_t LENGTH = static_cast<size_t>(1) << BITS;

    /** @brief 分配并清零固定大小的映射数组。 */
    PageMap1() {
        // 需要开辟数组的大小（字节）
        const size_t bytes = sizeof(std::atomic<void *>) * LENGTH;
        // 按页对齐后的大小（字节）
        const size_t aligned_bytes = (bytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        pages_ = aligned_bytes >> PAGE_SHIFT;

        array_ = static_cast<std::atomic<void *> *>(system_alloc(pages_));
        // mmap 只提供原始存储，须先建立原子对象的生命周期。
        for (size_t i = 0; i < LENGTH; ++i) {
            new (array_ + i) std::atomic<void *>(nullptr);
        }
    }

    /** @brief 归还构造时申请的映射数组。 */
    ~PageMap1() {
        if (array_ != nullptr) {
            system_free(array_, pages_);
            array_ = nullptr;
            pages_ = 0;
        }
    }

    PageMap1(const PageMap1 &) = delete;
    PageMap1 &operator=(const PageMap1 &) = delete;

    /** @brief 查询页号 k；超出本层可表示范围时返回 nullptr。 */
    void *get(Number k) const {
        if ((k >> BITS) > 0) {
            return nullptr;
        }
        return array_[static_cast<size_t>(k)].load(std::memory_order_acquire);
    }

    /** @brief 设置单个页号映射；页号必须在本层范围内。 */
    void set(Number k, void *v) {
        assert((k >> BITS) == 0);
        array_[static_cast<size_t>(k)].store(v, std::memory_order_release);
    }

    // 批量设置 [start, start+n-1] 的映射。
    // 适用于 Span 按页连续建映射的场景。
    /** @brief 将连续 n 个页号映射到同一指针。 */
    void set_range(Number start, size_t n, void *v) {
        if (n == 0) {
            return;
        }
        const bool ok = ensure(start, n);
        assert(ok);
        if (!ok) {
            return;
        }
        for (size_t i = 0; i < n; ++i) {
            array_[start + i].store(v, std::memory_order_release);
        }
    }

    // 清除已有映射不应分配 radix tree 节点。
    /** @brief 清除连续 n 个页号的映射。 */
    void clear_range(Number start, size_t n) {
        assert(ensure(start, n));
        for (size_t i = 0; i < n; ++i) {
            array_[start + i].store(nullptr, std::memory_order_release);
        }
    }

    /** @brief 检查连续页号范围是否可表示；固定数组不会额外分配节点。 */
    bool ensure(Number start, size_t n) {
        if (n == 0) {
            return true;
        }
        // [start, start+n-1] 必须落在 BITS 范围内。
        const Number last = start + static_cast<Number>(n - 1);
        return ((start >> BITS) == 0) && ((last >> BITS) == 0);
    }

  private:
    std::atomic<void *> *array_ = nullptr;
    size_t pages_ = 0;
};

/**
 * @brief 二层基数树（适用于 32 位系统）
 * @tparam BITS 页号位数
 */
template <int BITS> class PageMap2 {
  public:
    using Number = uintptr_t;

    /** @brief 初始化根层，并预建可表示地址范围的叶节点。 */
    PageMap2() {
        for (auto &entry : root_) {
            entry.store(nullptr, std::memory_order_relaxed);
        }
        preallocate_more_memory();
    }

    /** @brief 查询页号 k；未建叶节点或页号越界时返回 nullptr。 */
    void *get(Number k) const {
        const Number i1 = k >> LEAF_BITS;
        const Number i2 = k & (LEAF_LENGTH - 1);
        if ((k >> BITS) > 0) {
            return nullptr;
        }
        Leaf *leaf = root_[i1].load(std::memory_order_acquire);
        return leaf == nullptr
                   ? nullptr
                   : leaf->values[i2].load(std::memory_order_acquire);
    }

    /** @brief 设置单个页号映射；对应叶节点须已由 ensure 建立。 */
    void set(Number k, void *v) {
        const Number i1 = k >> LEAF_BITS;
        const Number i2 = k & (LEAF_LENGTH - 1);
        assert(i1 < ROOT_LENGTH);
        Leaf *leaf = root_[i1].load(std::memory_order_relaxed);
        leaf->values[i2].store(v, std::memory_order_release);
    }

    // 批量设置 [start, start+n-1] 的映射。
    /** @brief 将连续 n 个页号映射到同一指针，并按需建立叶节点。 */
    void set_range(Number start, size_t n, void *v) {
        if (n == 0) {
            return;
        }
        const bool ok = ensure(start, n);
        assert(ok);
        if (!ok) {
            return;
        }
        while (n != 0) {
            const Number i1 = start >> LEAF_BITS;
            const Number i2 = start & (LEAF_LENGTH - 1);
            const size_t count = std::min<size_t>(n, LEAF_LENGTH - i2);
            Leaf *leaf = root_[i1].load(std::memory_order_relaxed);
            for (size_t i = 0; i < count; ++i) {
                leaf->values[i2 + i].store(v, std::memory_order_release);
            }
            start += count;
            n -= count;
        }
    }

    /** @brief 清除连续 n 个页号的映射，不为缺失叶节点分配内存。 */
    void clear_range(Number start, size_t n) {
        while (n != 0) {
            const Number i1 = start >> LEAF_BITS;
            const Number i2 = start & (LEAF_LENGTH - 1);
            const size_t count = std::min<size_t>(n, LEAF_LENGTH - i2);
            assert(i1 < ROOT_LENGTH);
            Leaf *leaf = root_[i1].load(std::memory_order_relaxed);
            if (leaf != nullptr) {
                for (size_t i = 0; i < count; ++i) {
                    leaf->values[i2 + i].store(nullptr, std::memory_order_release);
                }
            }
            start += count;
            n -= count;
        }
    }

    /** @brief 确保连续页号范围的叶节点存在；越界时返回 false。 */
    bool ensure(Number start, size_t n) {
        if (n == 0) {
            return true;
        }
        for (Number key = start; key <= start + n - 1;) {
            const Number i1 = key >> LEAF_BITS;
            if (i1 >= ROOT_LENGTH) {
                return false;
            }
            if (root_[i1].load(std::memory_order_relaxed) == nullptr) {
                Leaf *leaf = leaf_pool_.allocate();
                for (auto &value : leaf->values) {
                    value.store(nullptr, std::memory_order_relaxed);
                }
                root_[i1].store(leaf, std::memory_order_release);
            }
            key = ((key >> LEAF_BITS) + 1) << LEAF_BITS;
        }
        return true;
    }

    /** @brief 预建整段页号范围对应的叶节点。 */
    void preallocate_more_memory() {
        ensure(0, static_cast<size_t>(1) << BITS);
    }

  private:
    static constexpr int ROOT_BITS = 5;
    static constexpr int ROOT_LENGTH = 1 << ROOT_BITS;
    static constexpr int LEAF_BITS = BITS - ROOT_BITS;
    static constexpr int LEAF_LENGTH = 1 << LEAF_BITS;

    struct Leaf {
        std::array<std::atomic<void *>, LEAF_LENGTH> values;
    };

    std::array<std::atomic<Leaf *>, ROOT_LENGTH> root_;
    ObjectPool<Leaf> leaf_pool_;
};

/**
 * @brief 三层基数树（适用于 64 位系统）
 * @tparam BITS 页号位数
 */
template <int BITS> class PageMap3 {
  public:
    using Number = uintptr_t;

    /** @brief 创建根节点；更深层节点在首次映射时按需建立。 */
    PageMap3() { root_ = new_node(); }

    /** @brief 查询页号 k；路径节点尚未建立或页号越界时返回 nullptr。 */
    void *get(Number k) const {
        const Number i1 = k >> (LEAF_BITS + INTERIOR_BITS);
        const Number i2 = (k >> LEAF_BITS) & (INTERIOR_LENGTH - 1);
        const Number i3 = k & (LEAF_LENGTH - 1);

        if ((k >> BITS) > 0) {
            return nullptr;
        }
        Node *node = root_->ptrs[i1].load(std::memory_order_acquire);
        if (node == nullptr) {
            return nullptr;
        }
        Leaf *leaf = reinterpret_cast<Leaf *>(
            node->ptrs[i2].load(std::memory_order_acquire));
        return leaf == nullptr
                   ? nullptr
                   : leaf->values[i3].load(std::memory_order_acquire);
    }

    /** @brief 设置单个页号映射，必要时建立对应的中间节点和叶节点。 */
    void set(Number k, void *v) {
        assert((k >> BITS) == 0);
        const Number i1 = k >> (LEAF_BITS + INTERIOR_BITS);
        const Number i2 = (k >> LEAF_BITS) & (INTERIOR_LENGTH - 1);
        const Number i3 = k & (LEAF_LENGTH - 1);

        Node *node = root_->ptrs[i1].load(std::memory_order_relaxed);
        if (node == nullptr ||
            node->ptrs[i2].load(std::memory_order_relaxed) == nullptr) {
            ensure(k, 1);
            node = root_->ptrs[i1].load(std::memory_order_relaxed);
        }
        Leaf *leaf = reinterpret_cast<Leaf *>(
            node->ptrs[i2].load(std::memory_order_relaxed));
        leaf->values[i3].store(v, std::memory_order_release);
    }

    // 批量设置 [start, start+n-1] 的映射。
    // 说明：set(k) 内部每次都会 ensure(k, 1)，对连续页映射来说开销较大。
    // 这里改成 ensure(start, n) 一次性建好节点/叶子，再逐页写入。
    /** @brief 将连续 n 个页号映射到同一指针，先批量建立所需节点。 */
    void set_range(Number start, size_t n, void *v) {
        if (n == 0) {
            return;
        }
        const bool ok = ensure(start, n);
        assert(ok);
        if (!ok) {
            return;
        }

        while (n != 0) {
            const Number i1 = start >> (LEAF_BITS + INTERIOR_BITS);
            const Number i2 =
                (start >> LEAF_BITS) & (INTERIOR_LENGTH - 1);
            const Number i3 = start & (LEAF_LENGTH - 1);
            const size_t count = std::min<size_t>(n, LEAF_LENGTH - i3);
            Node *node = root_->ptrs[i1].load(std::memory_order_relaxed);
            Leaf *leaf = reinterpret_cast<Leaf *>(
                node->ptrs[i2].load(std::memory_order_relaxed));
            for (size_t i = 0; i < count; ++i) {
                leaf->values[i3 + i].store(v, std::memory_order_release);
            }
            start += count;
            n -= count;
        }
    }

    /** @brief 清除连续 n 个页号的映射，不为缺失节点分配内存。 */
    void clear_range(Number start, size_t n) {
        while (n != 0) {
            const Number i1 = start >> (LEAF_BITS + INTERIOR_BITS);
            const Number i2 = (start >> LEAF_BITS) & (INTERIOR_LENGTH - 1);
            const Number i3 = start & (LEAF_LENGTH - 1);
            const size_t count = std::min<size_t>(n, LEAF_LENGTH - i3);
            assert(i1 < INTERIOR_LENGTH);
            Node *node = root_->ptrs[i1].load(std::memory_order_relaxed);
            if (node != nullptr) {
                Leaf *leaf = reinterpret_cast<Leaf *>(
                    node->ptrs[i2].load(std::memory_order_relaxed));
                if (leaf != nullptr) {
                    for (size_t i = 0; i < count; ++i) {
                        leaf->values[i3 + i].store(nullptr,
                                                 std::memory_order_release);
                    }
                }
            }
            start += count;
            n -= count;
        }
    }

    /** @brief 确保连续页号范围的路径节点存在；页号越界时返回 false。 */
    bool ensure(Number start, size_t n) {
        if (n == 0) {
            return true;
        }
        for (Number key = start; key <= start + n - 1;) {
            const Number i1 = key >> (LEAF_BITS + INTERIOR_BITS);
            const Number i2 = (key >> LEAF_BITS) & (INTERIOR_LENGTH - 1);

            if (i1 >= INTERIOR_LENGTH || i2 >= INTERIOR_LENGTH) {
                return false;
            }
            Node *node = root_->ptrs[i1].load(std::memory_order_relaxed);
            if (node == nullptr) {
                node = new_node();
                if (node == nullptr)
                    return false;
                root_->ptrs[i1].store(node, std::memory_order_release);
            }
            if (node->ptrs[i2].load(std::memory_order_relaxed) == nullptr) {
                Leaf *leaf = leaf_pool_.allocate();
                if (leaf == nullptr)
                    return false;
                for (auto &value : leaf->values) {
                    value.store(nullptr, std::memory_order_relaxed);
                }
                node->ptrs[i2].store(reinterpret_cast<Node *>(leaf),
                                    std::memory_order_release);
            }
            key = ((key >> LEAF_BITS) + 1) << LEAF_BITS;
        }
        return true;
    }

  private:
    static constexpr int INTERIOR_BITS = (BITS + 2) / 3;
    static constexpr int INTERIOR_LENGTH = 1 << INTERIOR_BITS;
    static constexpr int LEAF_BITS = BITS - 2 * INTERIOR_BITS;
    static constexpr int LEAF_LENGTH = 1 << LEAF_BITS;

    struct Node {
        std::array<std::atomic<Node *>, INTERIOR_LENGTH> ptrs;
    };

    struct Leaf {
        std::array<std::atomic<void *>, LEAF_LENGTH> values;
    };

    Node *new_node() {
        Node *result = node_pool_.allocate();
        if (result != nullptr) {
            for (auto &entry : result->ptrs) {
                entry.store(nullptr, std::memory_order_relaxed);
            }
        }
        return result;
    }

    Node *root_;
    ObjectPool<Node> node_pool_;
    ObjectPool<Leaf> leaf_pool_;
};

// 根据指针大小选择合适的基数树实现
#if __SIZEOF_POINTER__ == 4
// 32位系统使用二层基数树
using PageMap = PageMap2<32 - PAGE_SHIFT>;
#else
// 64位系统使用三层基数树
using PageMap = PageMap3<48 - PAGE_SHIFT>; // 大多数 64 位系统实际使用 48 位地址
#endif

} // namespace zmalloc

#endif // ZMALLOC_INTERNAL_PAGE_MAP_H_
