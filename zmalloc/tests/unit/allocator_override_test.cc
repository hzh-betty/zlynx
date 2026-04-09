#include "page_cache.h"

#include <cstdlib>
#include <new>

#include <gtest/gtest.h>

namespace zmalloc {
namespace {

TEST(AllocatorOverrideTest, MallocPointersAreTrackedByPageCache) {
  void *ptr = std::malloc(64);
  ASSERT_NE(ptr, nullptr);

  Span *span = PageCache::get_instance().map_object_to_span(ptr);
  EXPECT_NE(span, nullptr);
  EXPECT_GE(span->obj_size, 64u);

  std::free(ptr);
}

TEST(AllocatorOverrideTest, CallocZeroInitializesMemory) {
  auto *ptr = static_cast<unsigned char *>(std::calloc(32, sizeof(unsigned char)));
  ASSERT_NE(ptr, nullptr);

  Span *span = PageCache::get_instance().map_object_to_span(ptr);
  ASSERT_NE(span, nullptr);
  EXPECT_GE(span->obj_size, 32u);
  for (size_t i = 0; i < 32; ++i) {
    EXPECT_EQ(ptr[i], 0u);
  }

  std::free(ptr);
}

TEST(AllocatorOverrideTest, ReallocPreservesPrefixWhenGrowing) {
  auto *ptr = static_cast<unsigned char *>(std::malloc(32));
  ASSERT_NE(ptr, nullptr);
  for (size_t i = 0; i < 32; ++i) {
    ptr[i] = static_cast<unsigned char>(i);
  }

  auto *grown = static_cast<unsigned char *>(std::realloc(ptr, 128));
  ASSERT_NE(grown, nullptr);
  Span *span = PageCache::get_instance().map_object_to_span(grown);
  ASSERT_NE(span, nullptr);
  EXPECT_GE(span->obj_size, 128u);
  for (size_t i = 0; i < 32; ++i) {
    EXPECT_EQ(grown[i], static_cast<unsigned char>(i));
  }

  std::free(grown);
}

TEST(AllocatorOverrideTest, ReallocPreservesPrefixWhenShrinking) {
  auto *ptr = static_cast<unsigned char *>(std::malloc(128));
  ASSERT_NE(ptr, nullptr);
  for (size_t i = 0; i < 128; ++i) {
    ptr[i] = static_cast<unsigned char>(255 - i);
  }

  auto *shrunk = static_cast<unsigned char *>(std::realloc(ptr, 24));
  ASSERT_NE(shrunk, nullptr);
  Span *span = PageCache::get_instance().map_object_to_span(shrunk);
  ASSERT_NE(span, nullptr);
  EXPECT_GE(span->obj_size, 24u);
  for (size_t i = 0; i < 24; ++i) {
    EXPECT_EQ(shrunk[i], static_cast<unsigned char>(255 - i));
  }

  std::free(shrunk);
}

TEST(AllocatorOverrideTest, ReallocNullptrBehavesLikeMalloc) {
  void *ptr = std::realloc(nullptr, 96);
  ASSERT_NE(ptr, nullptr);

  Span *span = PageCache::get_instance().map_object_to_span(ptr);
  ASSERT_NE(span, nullptr);
  EXPECT_GE(span->obj_size, 96u);

  std::free(ptr);
}

TEST(AllocatorOverrideTest, ReallocZeroFreesAllocationAndReturnsNull) {
  void *ptr = std::malloc(64);
  ASSERT_NE(ptr, nullptr);

  EXPECT_EQ(std::realloc(ptr, 0), nullptr);
}

TEST(AllocatorOverrideTest, OperatorNewIsTrackedByPageCache) {
  auto *ptr = new unsigned char[48];
  ASSERT_NE(ptr, nullptr);

  Span *span = PageCache::get_instance().map_object_to_span(ptr);
  ASSERT_NE(span, nullptr);
  EXPECT_GE(span->obj_size, 48u);

  delete[] ptr;
}

TEST(AllocatorOverrideTest, NothrowNewReturnsManagedPointer) {
  auto *ptr = new (std::nothrow) unsigned char[80];
  ASSERT_NE(ptr, nullptr);

  Span *span = PageCache::get_instance().map_object_to_span(ptr);
  ASSERT_NE(span, nullptr);
  EXPECT_GE(span->obj_size, 80u);

  delete[] ptr;
}

} // namespace
} // namespace zmalloc
