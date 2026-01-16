#pragma once

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

#include "common.h"
#include "object_pool.h"

namespace zmalloc {

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

  bool ensure(Number start, size_t n) {
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

  bool ensure(Number start, size_t n) {
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
