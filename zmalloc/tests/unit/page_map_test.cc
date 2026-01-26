#include <gtest/gtest.h>

#include <cstdint>

#include "common.h"
#include "page_map.h"

namespace {

template <typename PM> void SmokeSetGet(PM &pm, uintptr_t k1, uintptr_t k2) {
  int a = 1;
  int b = 2;
  EXPECT_EQ(pm.get(k1), nullptr);
  pm.set(k1, &a);
  pm.set(k2, &b);
  EXPECT_EQ(pm.get(k1), &a);
  EXPECT_EQ(pm.get(k2), &b);
}

template <typename PM>
static void SetManyAndCheckSome(PM &pm, uintptr_t start, uintptr_t step,
                                int count) {
  static constexpr int kMax = 64;
  ASSERT_LE(count, kMax);
  int values[kMax];
  for (int i = 0; i < count; ++i) {
    values[i] = i + 100;
    const uintptr_t key = start + static_cast<uintptr_t>(i) * step;
    pm.set(key, &values[i]);
  }

  // 检查头/中/尾
  const uintptr_t k0 = start;
  const uintptr_t km = start + static_cast<uintptr_t>(count / 2) * step;
  const uintptr_t k1 = start + static_cast<uintptr_t>(count - 1) * step;
  EXPECT_EQ(pm.get(k0), &values[0]);
  EXPECT_EQ(pm.get(km), &values[count / 2]);
  EXPECT_EQ(pm.get(k1), &values[count - 1]);
}

} // namespace

class PageMap1Test : public ::testing::Test {
protected:
  zmalloc::PageMap1<12> pm;
};

TEST_F(PageMap1Test, DefaultNull) {
  EXPECT_EQ(pm.get(0), nullptr);
  EXPECT_EQ(pm.get(1), nullptr);
}

TEST_F(PageMap1Test, SetGetSingle) {
  int v = 123;
  pm.set(7, &v);
  EXPECT_EQ(pm.get(7), &v);
}

TEST_F(PageMap1Test, SetGetTwoKeys) { SmokeSetGet(pm, 1, 4095); }

TEST_F(PageMap1Test, EnsureZeroIsOk) {
  EXPECT_TRUE(pm.ensure(0, 0));
  EXPECT_TRUE(pm.ensure(10, 0));
}

TEST_F(PageMap1Test, EnsureInRange) {
  EXPECT_TRUE(pm.ensure(0, 1));
  EXPECT_TRUE(pm.ensure(4095, 1));
  EXPECT_TRUE(pm.ensure(100, 200));
}

TEST_F(PageMap1Test, EnsureOutOfRange) {
  EXPECT_FALSE(pm.ensure(4096, 1));
  EXPECT_FALSE(pm.ensure(4095, 2));
}

TEST_F(PageMap1Test, GetOutOfRangeReturnsNull) {
  EXPECT_EQ(pm.get(4096), nullptr);
  EXPECT_EQ(pm.get(8192), nullptr);
}

TEST_F(PageMap1Test, SetGetMinKey) {
  int v = 1;
  pm.set(0, &v);
  EXPECT_EQ(pm.get(0), &v);
}

TEST_F(PageMap1Test, SetGetMaxKey) {
  int v = 2;
  pm.set(4095, &v);
  EXPECT_EQ(pm.get(4095), &v);
}

TEST_F(PageMap1Test, OverwriteSameKey) {
  int a = 1;
  int b = 2;
  pm.set(10, &a);
  EXPECT_EQ(pm.get(10), &a);
  pm.set(10, &b);
  EXPECT_EQ(pm.get(10), &b);
}

TEST_F(PageMap1Test, ZeroInitializedAllNullForSampleKeys) {
  EXPECT_EQ(pm.get(0), nullptr);
  EXPECT_EQ(pm.get(7), nullptr);
  EXPECT_EQ(pm.get(100), nullptr);
  EXPECT_EQ(pm.get(4095), nullptr);
}

TEST_F(PageMap1Test, EnsureFullRangeInRange) {
  EXPECT_TRUE(pm.ensure(0, 4096));
}

TEST_F(PageMap1Test, EnsureFullRangeStartingAtOneOutOfRange) {
  EXPECT_FALSE(pm.ensure(1, 4096));
}

TEST_F(PageMap1Test, EnsureStartOutOfRangeWithZeroLengthIsOk) {
  // n==0 允许；这是接口约定。
  EXPECT_TRUE(pm.ensure(4096, 0));
}

TEST_F(PageMap1Test, EnsureCrossesEndIsOutOfRange) {
  EXPECT_FALSE(pm.ensure(4094, 3));
}

TEST_F(PageMap1Test, ManySequentialSets) { SetManyAndCheckSome(pm, 0, 1, 32); }

TEST_F(PageMap1Test, ManyStrideSets) { SetManyAndCheckSome(pm, 1, 7, 32); }

TEST_F(PageMap1Test, SparseSetsDoNotAffectOthers) {
  int a = 11;
  int b = 22;
  pm.set(100, &a);
  pm.set(2000, &b);
  EXPECT_EQ(pm.get(101), nullptr);
  EXPECT_EQ(pm.get(1999), nullptr);
  EXPECT_EQ(pm.get(2001), nullptr);
}

TEST_F(PageMap1Test, SettingOneKeyDoesNotChangeAnother) {
  int a = 1;
  pm.set(123, &a);
  EXPECT_EQ(pm.get(124), nullptr);
  EXPECT_EQ(pm.get(122), nullptr);
}

TEST_F(PageMap1Test, SetManyIncludingEnds) {
  int a = 1;
  int b = 2;
  int c = 3;
  pm.set(0, &a);
  pm.set(1, &b);
  pm.set(4095, &c);
  EXPECT_EQ(pm.get(0), &a);
  EXPECT_EQ(pm.get(1), &b);
  EXPECT_EQ(pm.get(4095), &c);
}

TEST_F(PageMap1Test, EnsureOneElementAtEndInRange) {
  EXPECT_TRUE(pm.ensure(4095, 1));
}

TEST_F(PageMap1Test, EnsureTwoElementsAtEndOutOfRange) {
  EXPECT_FALSE(pm.ensure(4095, 2));
}

class PageMap2Test : public ::testing::Test {
protected:
  zmalloc::PageMap2<16> pm;
};

TEST_F(PageMap2Test, DefaultNull) {
  EXPECT_EQ(pm.get(0), nullptr);
  EXPECT_EQ(pm.get(12345), nullptr);
}

TEST_F(PageMap2Test, SetGet) { SmokeSetGet(pm, 1, 65535); }

TEST_F(PageMap2Test, EnsureInRange) {
  EXPECT_TRUE(pm.ensure(0, 1));
  EXPECT_TRUE(pm.ensure(65535, 1));
}

TEST_F(PageMap2Test, EnsureOutOfRange) { EXPECT_FALSE(pm.ensure(65536, 1)); }

TEST_F(PageMap2Test, GetOutOfRangeReturnsNull) {
  EXPECT_EQ(pm.get(65536), nullptr);
  EXPECT_EQ(pm.get(1u << 20), nullptr);
}

TEST_F(PageMap2Test, SetGetMinKey) {
  int v = 1;
  pm.set(0, &v);
  EXPECT_EQ(pm.get(0), &v);
}

TEST_F(PageMap2Test, SetGetMaxKey) {
  int v = 2;
  pm.set(65535, &v);
  EXPECT_EQ(pm.get(65535), &v);
}

TEST_F(PageMap2Test, OverwriteSameKey) {
  int a = 1;
  int b = 2;
  pm.set(1234, &a);
  EXPECT_EQ(pm.get(1234), &a);
  pm.set(1234, &b);
  EXPECT_EQ(pm.get(1234), &b);
}

TEST_F(PageMap2Test, LeafBoundaryKeysIndependent) {
  // PageMap2<16> 的 leaf size 为 2^(16-5)=2048，边界为 2047/2048。
  int a = 1;
  int b = 2;
  pm.set(2047, &a);
  pm.set(2048, &b);
  EXPECT_EQ(pm.get(2047), &a);
  EXPECT_EQ(pm.get(2048), &b);
}

TEST_F(PageMap2Test, TwoDifferentLeafBoundaries) {
  int a = 1;
  int b = 2;
  int c = 3;
  int d = 4;
  pm.set(0, &a);
  pm.set(2048, &b);
  pm.set(4095, &c);
  pm.set(4096, &d);
  EXPECT_EQ(pm.get(0), &a);
  EXPECT_EQ(pm.get(2048), &b);
  EXPECT_EQ(pm.get(4095), &c);
  EXPECT_EQ(pm.get(4096), &d);
}

TEST_F(PageMap2Test, EnsureAcrossLeafBoundary) {
  EXPECT_TRUE(pm.ensure(2047, 2));
  EXPECT_TRUE(pm.ensure(2048, 2));
}

TEST_F(PageMap2Test, EnsureLargeRangeInRange) {
  EXPECT_TRUE(pm.ensure(0, 65536));
}

TEST_F(PageMap2Test, EnsureLargeRangeOutOfRange) {
  EXPECT_FALSE(pm.ensure(1, 65536));
}

TEST_F(PageMap2Test, ManySequentialSets) { SetManyAndCheckSome(pm, 0, 1, 48); }

TEST_F(PageMap2Test, ManyStrideSets) { SetManyAndCheckSome(pm, 7, 97, 48); }

TEST_F(PageMap2Test, SparseSetsAndNullGaps) {
  int a = 1;
  int b = 2;
  pm.set(1, &a);
  pm.set(65000, &b);
  EXPECT_EQ(pm.get(2), nullptr);
  EXPECT_EQ(pm.get(64999), nullptr);
  EXPECT_EQ(pm.get(65001), nullptr);
}

TEST_F(PageMap2Test, EnsureZeroLengthAlwaysTrue) {
  EXPECT_TRUE(pm.ensure(0, 0));
  EXPECT_TRUE(pm.ensure(999999, 0));
}

TEST_F(PageMap2Test, SetManyIncludingEnds) {
  int a = 1;
  int b = 2;
  int c = 3;
  pm.set(0, &a);
  pm.set(1, &b);
  pm.set(65535, &c);
  EXPECT_EQ(pm.get(0), &a);
  EXPECT_EQ(pm.get(1), &b);
  EXPECT_EQ(pm.get(65535), &c);
}

class PageMap3Test : public ::testing::Test {
protected:
  zmalloc::PageMap3<18> pm;
};

TEST_F(PageMap3Test, DefaultNull) {
  EXPECT_EQ(pm.get(0), nullptr);
  EXPECT_EQ(pm.get(42), nullptr);
}

TEST_F(PageMap3Test, SetGet) { SmokeSetGet(pm, 1, (1u << 18) - 1); }

TEST_F(PageMap3Test, EnsureInRange) {
  EXPECT_TRUE(pm.ensure(0, 1));
  EXPECT_TRUE(pm.ensure((1u << 18) - 1, 1));
  EXPECT_TRUE(pm.ensure(1000, 2000));
}

TEST_F(PageMap3Test, EnsureOutOfRange) {
  EXPECT_FALSE(pm.ensure(1u << 18, 1));
  EXPECT_FALSE(pm.ensure((1u << 18) - 1, 2));
}

TEST_F(PageMap3Test, ManySetsSparse) {
  int a = 1;
  int b = 2;
  int c = 3;
  pm.set(1, &a);
  pm.set(1000, &b);
  pm.set((1u << 18) - 1, &c);
  EXPECT_EQ(pm.get(1), &a);
  EXPECT_EQ(pm.get(1000), &b);
  EXPECT_EQ(pm.get((1u << 18) - 1), &c);
}

TEST_F(PageMap3Test, GetOutOfRangeReturnsNull) {
  EXPECT_EQ(pm.get(1u << 18), nullptr);
  EXPECT_EQ(pm.get((1u << 20) + 3), nullptr);
}

TEST_F(PageMap3Test, SetGetMinKey) {
  int v = 1;
  pm.set(0, &v);
  EXPECT_EQ(pm.get(0), &v);
}

TEST_F(PageMap3Test, SetGetMaxKey) {
  int v = 2;
  pm.set((1u << 18) - 1, &v);
  EXPECT_EQ(pm.get((1u << 18) - 1), &v);
}

TEST_F(PageMap3Test, OverwriteSameKey) {
  int a = 1;
  int b = 2;
  pm.set(12345, &a);
  EXPECT_EQ(pm.get(12345), &a);
  pm.set(12345, &b);
  EXPECT_EQ(pm.get(12345), &b);
}

TEST_F(PageMap3Test, LeafBoundaryKeysIndependent) {
  // PageMap3<18> 下 leaf size = 64，边界为 63/64。
  int a = 1;
  int b = 2;
  pm.set(63, &a);
  pm.set(64, &b);
  EXPECT_EQ(pm.get(63), &a);
  EXPECT_EQ(pm.get(64), &b);
}

TEST_F(PageMap3Test, InteriorBoundaryKeysIndependent) {
  // i2 边界为 4095/4096
  int a = 1;
  int b = 2;
  pm.set(4095, &a);
  pm.set(4096, &b);
  EXPECT_EQ(pm.get(4095), &a);
  EXPECT_EQ(pm.get(4096), &b);
}

TEST_F(PageMap3Test, MultipleBoundaries) {
  int a = 1;
  int b = 2;
  int c = 3;
  int d = 4;
  pm.set(0, &a);
  pm.set(63, &b);
  pm.set(64, &c);
  pm.set(4096, &d);
  EXPECT_EQ(pm.get(0), &a);
  EXPECT_EQ(pm.get(63), &b);
  EXPECT_EQ(pm.get(64), &c);
  EXPECT_EQ(pm.get(4096), &d);
}

TEST_F(PageMap3Test, EnsureAcrossLeafBoundary) {
  EXPECT_TRUE(pm.ensure(63, 2));
  EXPECT_TRUE(pm.ensure(64, 2));
}

TEST_F(PageMap3Test, EnsureAcrossInteriorBoundary) {
  EXPECT_TRUE(pm.ensure(4095, 2));
  EXPECT_TRUE(pm.ensure(4096, 2));
}

TEST_F(PageMap3Test, EnsureLargeRangeInRange) {
  EXPECT_TRUE(pm.ensure(0, 1u << 18));
}

TEST_F(PageMap3Test, EnsureLargeRangeOutOfRange) {
  EXPECT_FALSE(pm.ensure(1, 1u << 18));
}

TEST_F(PageMap3Test, ManySequentialSets) { SetManyAndCheckSome(pm, 0, 1, 60); }

TEST_F(PageMap3Test, ManyStrideSets) { SetManyAndCheckSome(pm, 5, 97, 60); }

TEST_F(PageMap3Test, SparseSetsAndNullGaps) {
  int a = 1;
  int b = 2;
  pm.set(1, &a);
  pm.set((1u << 18) - 2, &b);
  EXPECT_EQ(pm.get(2), nullptr);
  EXPECT_EQ(pm.get((1u << 18) - 3), nullptr);
  EXPECT_EQ(pm.get((1u << 18) - 1), nullptr);
}

TEST_F(PageMap3Test, EnsureZeroLengthAlwaysTrue) {
  EXPECT_TRUE(pm.ensure(0, 0));
  EXPECT_TRUE(pm.ensure(999999, 0));
}

TEST_F(PageMap3Test, SetManyIncludingEnds) {
  int a = 1;
  int b = 2;
  int c = 3;
  pm.set(0, &a);
  pm.set(1, &b);
  pm.set((1u << 18) - 1, &c);
  EXPECT_EQ(pm.get(0), &a);
  EXPECT_EQ(pm.get(1), &b);
  EXPECT_EQ(pm.get((1u << 18) - 1), &c);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
