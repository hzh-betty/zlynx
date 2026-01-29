#ifndef ZMALLOC_PAGE_MAP_H_
#define ZMALLOC_PAGE_MAP_H_

/**
 * @file page_map.h
 * @brief 基数树实现，用于页号到 Span 的高效映射
 *
 * X86 (32位): 二层基数树 PageMap2
 * X64 (64位): 三层基数树 PageMap3
 */

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

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
  static_assert(BITS <= 20, "PageMap1 is only intended for small BITS (<=20)");

  static constexpr size_t LENGTH = static_cast<size_t>(1) << BITS;

  PageMap1() {
    // 需要开辟数组的大小（字节）
    const size_t bytes = sizeof(void *) * LENGTH;
    // 按页对齐后的大小（字节）
    const size_t aligned_bytes = (bytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    pages_ = aligned_bytes >> PAGE_SHIFT;

    array_ = static_cast<void **>(system_alloc(pages_));
    std::memset(array_, 0, bytes);
  }

  ~PageMap1() {
    if (array_ != nullptr) {
      system_free(array_, pages_);
      array_ = nullptr;
      pages_ = 0;
    }
  }

  PageMap1(const PageMap1 &) = delete;
  PageMap1 &operator=(const PageMap1 &) = delete;

  void *get(Number k) const {
    if ((k >> BITS) > 0) {
      return nullptr;
    }
    return array_[static_cast<size_t>(k)];
  }

  void set(Number k, void *v) {
    assert((k >> BITS) == 0);
    array_[static_cast<size_t>(k)] = v;
  }

  // 批量设置 [start, start+n-1] 的映射。
  // 适用于 Span 按页连续建映射的场景。
  void set_range(Number start, size_t n, void *v) {
    if (n == 0) {
      return;
    }
    assert(ensure(start, n));
    const Number last = start + static_cast<Number>(n - 1);
    for (Number k = start; k <= last; ++k) {
      array_[static_cast<size_t>(k)] = v;
    }
  }

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

  PageMap2() {
    std::memset(root_, 0, sizeof(root_));
    preallocate_more_memory();
  }

  void *get(Number k) const {
    const Number i1 = k >> LEAF_BITS;
    const Number i2 = k & (LEAF_LENGTH - 1);
    if ((k >> BITS) > 0 || root_[i1] == nullptr) {
      return nullptr;
    }
    return root_[i1]->values[i2];
  }

  void set(Number k, void *v) {
    const Number i1 = k >> LEAF_BITS;
    const Number i2 = k & (LEAF_LENGTH - 1);
    assert(i1 < ROOT_LENGTH);
    root_[i1]->values[i2] = v;
  }

  // 批量设置 [start, start+n-1] 的映射。
  void set_range(Number start, size_t n, void *v) {
    if (n == 0) {
      return;
    }
    assert(ensure(start, n));
    const Number last = start + static_cast<Number>(n - 1);
    for (Number k = start; k <= last; ++k) {
      const Number i1 = k >> LEAF_BITS;
      const Number i2 = k & (LEAF_LENGTH - 1);
      root_[i1]->values[i2] = v;
    }
  }

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
        std::memset(leaf, 0, sizeof(*leaf));
        root_[i1] = leaf;
      }
      key = ((key >> LEAF_BITS) + 1) << LEAF_BITS;
    }
    return true;
  }

  void preallocate_more_memory() { ensure(0, static_cast<size_t>(1) << BITS); }

private:
  static constexpr int ROOT_BITS = 5;
  static constexpr int ROOT_LENGTH = 1 << ROOT_BITS;
  static constexpr int LEAF_BITS = BITS - ROOT_BITS;
  static constexpr int LEAF_LENGTH = 1 << LEAF_BITS;

  struct Leaf {
    void *values[LEAF_LENGTH];
  };

  Leaf *root_[ROOT_LENGTH];
  ObjectPool<Leaf> leaf_pool_;
};

/**
 * @brief 三层基数树（适用于 64 位系统）
 * @tparam BITS 页号位数
 */
template <int BITS> class PageMap3 {
public:
  using Number = uintptr_t;

  PageMap3() { root_ = new_node(); }

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

  void set(Number k, void *v) {
    assert((k >> BITS) == 0);
    const Number i1 = k >> (LEAF_BITS + INTERIOR_BITS);
    const Number i2 = (k >> LEAF_BITS) & (INTERIOR_LENGTH - 1);
    const Number i3 = k & (LEAF_LENGTH - 1);

    ensure(k, 1);
    reinterpret_cast<Leaf *>(root_->ptrs[i1]->ptrs[i2])->values[i3] = v;
  }

  // 批量设置 [start, start+n-1] 的映射。
  // 说明：set(k) 内部每次都会 ensure(k, 1)，对连续页映射来说开销较大。
  // 这里改成 ensure(start, n) 一次性建好节点/叶子，再逐页写入。
  void set_range(Number start, size_t n, void *v) {
    if (n == 0) {
      return;
    }
    assert(ensure(start, n));

    const Number last = start + static_cast<Number>(n - 1);
    for (Number k = start; k <= last; ++k) {
      const Number i1 = k >> (LEAF_BITS + INTERIOR_BITS);
      const Number i2 = (k >> LEAF_BITS) & (INTERIOR_LENGTH - 1);
      const Number i3 = k & (LEAF_LENGTH - 1);
      reinterpret_cast<Leaf *>(root_->ptrs[i1]->ptrs[i2])->values[i3] = v;
    }
  }

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
        std::memset(leaf, 0, sizeof(*leaf));
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
    Node *ptrs[INTERIOR_LENGTH];
  };

  struct Leaf {
    void *values[LEAF_LENGTH];
  };

  Node *new_node() {
    Node *result = node_pool_.allocate();
    if (result != nullptr) {
      std::memset(result, 0, sizeof(*result));
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

#endif // ZMALLOC_PAGE_MAP_H_
