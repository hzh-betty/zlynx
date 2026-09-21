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
#include <cassert>
#include <cstddef>
#include <cstdint>

#include "object_pool.h"
#include "system_alloc.h"
#include "zmalloc_config.h"

namespace zmalloc {

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
        const size_t bytes = sizeof(void *) * LENGTH;
        // 按页对齐后的大小（字节）
        const size_t aligned_bytes = (bytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        pages_ = aligned_bytes >> PAGE_SHIFT;

        array_ = static_cast<void **>(system_alloc(pages_));
        std::fill_n(array_, LENGTH, nullptr);
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
        return array_[static_cast<size_t>(k)];
    }

    /** @brief 设置单个页号映射；页号必须在本层范围内。 */
    void set(Number k, void *v) {
        assert((k >> BITS) == 0);
        array_[static_cast<size_t>(k)] = v;
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
        std::fill_n(array_ + static_cast<size_t>(start), n, v);
    }

    // 清除已有映射不应分配 radix tree 节点。
    /** @brief 清除连续 n 个页号的映射。 */
    void clear_range(Number start, size_t n) {
        assert(ensure(start, n));
        std::fill_n(array_ + start, n, nullptr);
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
    void **array_ = nullptr;
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
        root_.fill(nullptr);
        preallocate_more_memory();
    }

    /** @brief 查询页号 k；未建叶节点或页号越界时返回 nullptr。 */
    void *get(Number k) const {
        const Number i1 = k >> LEAF_BITS;
        const Number i2 = k & (LEAF_LENGTH - 1);
        if ((k >> BITS) > 0 || root_[i1] == nullptr) {
            return nullptr;
        }
        return root_[i1]->values[i2];
    }

    /** @brief 设置单个页号映射；对应叶节点须已由 ensure 建立。 */
    void set(Number k, void *v) {
        const Number i1 = k >> LEAF_BITS;
        const Number i2 = k & (LEAF_LENGTH - 1);
        assert(i1 < ROOT_LENGTH);
        root_[i1]->values[i2] = v;
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
            std::fill_n(root_[i1]->values.data() + i2, count, v);
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
            if (root_[i1] != nullptr) {
                std::fill_n(root_[i1]->values.data() + i2, count, nullptr);
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
            if (root_[i1] == nullptr) {
                Leaf *leaf = leaf_pool_.allocate();
                leaf->values.fill(nullptr);
                root_[i1] = leaf;
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
        std::array<void *, LEAF_LENGTH> values;
    };

    std::array<Leaf *, ROOT_LENGTH> root_;
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

        if ((k >> BITS) > 0 || root_->ptrs[i1] == nullptr ||
            root_->ptrs[i1]->ptrs[i2] == nullptr) {
            return nullptr;
        }
        return reinterpret_cast<Leaf *>(root_->ptrs[i1]->ptrs[i2])->values[i3];
    }

    /** @brief 设置单个页号映射，必要时建立对应的中间节点和叶节点。 */
    void set(Number k, void *v) {
        assert((k >> BITS) == 0);
        const Number i1 = k >> (LEAF_BITS + INTERIOR_BITS);
        const Number i2 = (k >> LEAF_BITS) & (INTERIOR_LENGTH - 1);
        const Number i3 = k & (LEAF_LENGTH - 1);

        if (root_->ptrs[i1] == nullptr ||
            root_->ptrs[i1]->ptrs[i2] == nullptr) {
            ensure(k, 1);
        }
        reinterpret_cast<Leaf *>(root_->ptrs[i1]->ptrs[i2])->values[i3] = v;
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
            Leaf *leaf =
                reinterpret_cast<Leaf *>(root_->ptrs[i1]->ptrs[i2]);
            std::fill_n(leaf->values.data() + i3, count, v);
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
            if (root_->ptrs[i1] != nullptr &&
                root_->ptrs[i1]->ptrs[i2] != nullptr) {
                Leaf *leaf =
                    reinterpret_cast<Leaf *>(root_->ptrs[i1]->ptrs[i2]);
                std::fill_n(leaf->values.data() + i3, count, nullptr);
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
            if (root_->ptrs[i1] == nullptr) {
                Node *node = new_node();
                if (node == nullptr)
                    return false;
                root_->ptrs[i1] = node;
            }
            if (root_->ptrs[i1]->ptrs[i2] == nullptr) {
                Leaf *leaf = leaf_pool_.allocate();
                if (leaf == nullptr)
                    return false;
                leaf->values.fill(nullptr);
                root_->ptrs[i1]->ptrs[i2] = reinterpret_cast<Node *>(leaf);
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
        std::array<Node *, INTERIOR_LENGTH> ptrs;
    };

    struct Leaf {
        std::array<void *, LEAF_LENGTH> values;
    };

    Node *new_node() {
        Node *result = node_pool_.allocate();
        if (result != nullptr) {
            result->ptrs.fill(nullptr);
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
