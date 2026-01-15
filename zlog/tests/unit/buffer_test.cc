#include "buffer.h"
#include <cstring>
#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace zlog;

class BufferTest : public ::testing::Test {
protected:
  void SetUp() override {}
  void TearDown() override {}
  Buffer buf;
};

// ===================== 初始状态测试 =====================

TEST_F(BufferTest, InitialState) {
  EXPECT_EQ(buf.readAbleSize(), 0u);
  EXPECT_EQ(buf.writeAbleSize(), DEFAULT_BUFFER_SIZE);
  EXPECT_TRUE(buf.empty());
  EXPECT_EQ(buf.capacity(), DEFAULT_BUFFER_SIZE);
  EXPECT_NE(buf.begin(), static_cast<const char *>(NULL));
}

TEST_F(BufferTest, DefaultBufferConstants) {
  EXPECT_EQ(DEFAULT_BUFFER_SIZE, 1024u * 1024u * 2u);   // 2MB
  EXPECT_EQ(THRESHOLD_BUFFER_SIZE, 1024u * 1024u * 8u); // 8MB
  EXPECT_EQ(INCREMENT_BUFFER_SIZE, 1024u * 1024u * 1u); // 1MB
  EXPECT_EQ(MAX_BUFFER_SIZE, 1024u * 1024u * 512u);     // 512MB
}

// ===================== Push操作测试 =====================

TEST_F(BufferTest, PushSmallData) {
  const char *data = "hello";
  buf.push(data, 5);

  EXPECT_EQ(buf.readAbleSize(), 5u);
  EXPECT_FALSE(buf.empty());
  EXPECT_EQ(std::string(buf.begin(), buf.readAbleSize()), "hello");
}

TEST_F(BufferTest, PushEmptyData) {
  buf.push("", 0);

  EXPECT_EQ(buf.readAbleSize(), 0u);
  EXPECT_TRUE(buf.empty());
}

TEST_F(BufferTest, PushSingleByte) {
  buf.push("X", 1);

  EXPECT_EQ(buf.readAbleSize(), 1u);
  EXPECT_EQ(buf.begin()[0], 'X');
}

TEST_F(BufferTest, PushLargeData) {
  std::vector<char> largeData(1024 * 1024, 'A'); // 1MB
  buf.push(largeData.data(), largeData.size());

  EXPECT_EQ(buf.readAbleSize(), largeData.size());
  EXPECT_EQ(std::memcmp(buf.begin(), largeData.data(), largeData.size()), 0);
}

TEST_F(BufferTest, PushMultipleTimes) {
  buf.push("hello", 5);
  buf.push(" ", 1);
  buf.push("world", 5);

  EXPECT_EQ(buf.readAbleSize(), 11u);
  EXPECT_EQ(std::string(buf.begin(), buf.readAbleSize()), "hello world");
}

TEST_F(BufferTest, PushBinaryData) {
  char binary[] = {0x00, 0x01, 0x02, static_cast<char>(0xFF),
                   static_cast<char>(0xFE)};
  buf.push(binary, 5);

  EXPECT_EQ(buf.readAbleSize(), 5u);
  EXPECT_EQ(std::memcmp(buf.begin(), binary, 5), 0);
}

TEST_F(BufferTest, PushWithNullBytes) {
  const char data[] = "hello\0world";
  buf.push(data, 11);

  EXPECT_EQ(buf.readAbleSize(), 11u);
  EXPECT_EQ(std::memcmp(buf.begin(), data, 11), 0);
}

// ===================== MoveReader测试 =====================

TEST_F(BufferTest, MoveReaderPartial) {
  buf.push("hello world", 11);
  buf.moveReader(6);

  EXPECT_EQ(buf.readAbleSize(), 5u);
  EXPECT_EQ(std::string(buf.begin(), buf.readAbleSize()), "world");
}

TEST_F(BufferTest, MoveReaderAll) {
  buf.push("hello", 5);
  buf.moveReader(5);

  EXPECT_EQ(buf.readAbleSize(), 0u);
  EXPECT_TRUE(buf.empty());
}

TEST_F(BufferTest, MoveReaderZero) {
  buf.push("hello", 5);
  buf.moveReader(0);

  EXPECT_EQ(buf.readAbleSize(), 5u);
  EXPECT_EQ(std::string(buf.begin(), buf.readAbleSize()), "hello");
}

TEST_F(BufferTest, MoveReaderMultipleTimes) {
  buf.push("abcdefghij", 10);

  buf.moveReader(2);
  EXPECT_EQ(std::string(buf.begin(), buf.readAbleSize()), "cdefghij");

  buf.moveReader(3);
  EXPECT_EQ(std::string(buf.begin(), buf.readAbleSize()), "fghij");

  buf.moveReader(5);
  EXPECT_TRUE(buf.empty());
}

// ===================== Reset测试 =====================

TEST_F(BufferTest, ResetAfterPush) {
  buf.push("hello world", 11);
  buf.reset();

  EXPECT_EQ(buf.readAbleSize(), 0u);
  EXPECT_TRUE(buf.empty());
  EXPECT_EQ(buf.writeAbleSize(), buf.capacity());
}

TEST_F(BufferTest, ResetAfterPartialRead) {
  buf.push("hello world", 11);
  buf.moveReader(5);
  buf.reset();

  EXPECT_EQ(buf.readAbleSize(), 0u);
  EXPECT_TRUE(buf.empty());
}

TEST_F(BufferTest, ResetOnEmpty) {
  buf.reset();

  EXPECT_EQ(buf.readAbleSize(), 0u);
  EXPECT_TRUE(buf.empty());
}

TEST_F(BufferTest, PushAfterReset) {
  buf.push("first", 5);
  buf.reset();
  buf.push("second", 6);

  EXPECT_EQ(buf.readAbleSize(), 6u);
  EXPECT_EQ(std::string(buf.begin(), buf.readAbleSize()), "second");
}

// ===================== Swap测试 =====================

TEST_F(BufferTest, SwapBothNonEmpty) {
  Buffer b2;
  buf.push("hello", 5);
  b2.push("world", 5);

  buf.swap(b2);

  EXPECT_EQ(std::string(buf.begin(), buf.readAbleSize()), "world");
  EXPECT_EQ(std::string(b2.begin(), b2.readAbleSize()), "hello");
}

TEST_F(BufferTest, SwapWithEmpty) {
  Buffer b2;
  buf.push("hello", 5);

  buf.swap(b2);

  EXPECT_TRUE(buf.empty());
  EXPECT_EQ(std::string(b2.begin(), b2.readAbleSize()), "hello");
}

TEST_F(BufferTest, SwapBothEmpty) {
  Buffer b2;
  buf.swap(b2);

  EXPECT_TRUE(buf.empty());
  EXPECT_TRUE(b2.empty());
}

TEST_F(BufferTest, SwapDifferentSizes) {
  Buffer b2;
  std::vector<char> largeData(1024 * 1024 * 3, 'X'); // 3MB，触发扩容
  buf.push(largeData.data(), largeData.size());
  b2.push("small", 5);

  size_t bufCap = buf.capacity();
  size_t b2Cap = b2.capacity();

  buf.swap(b2);

  EXPECT_EQ(buf.capacity(), b2Cap);
  EXPECT_EQ(b2.capacity(), bufCap);
  EXPECT_EQ(buf.readAbleSize(), 5u);
  EXPECT_EQ(b2.readAbleSize(), largeData.size());
}

TEST_F(BufferTest, SwapSelf) {
  buf.push("hello", 5);
  buf.swap(buf);

  EXPECT_EQ(std::string(buf.begin(), buf.readAbleSize()), "hello");
}

// ===================== 扩容机制测试 =====================

TEST_F(BufferTest, AutoResizeSmall) {
  std::vector<char> data(DEFAULT_BUFFER_SIZE + 1, 'A');
  buf.push(data.data(), data.size());

  EXPECT_GT(buf.capacity(), DEFAULT_BUFFER_SIZE);
  EXPECT_EQ(buf.readAbleSize(), data.size());
}

TEST_F(BufferTest, AutoResizeLarge) {
  std::vector<char> data(1024 * 1024 * 3, 'A'); // 3MB > 2MB
  EXPECT_NO_THROW(buf.push(data.data(), data.size()));
  EXPECT_EQ(buf.readAbleSize(), data.size());
}

TEST_F(BufferTest, ResizeBelowThreshold) {
  // 低于阈值时容量翻倍
  size_t origCap = buf.capacity();
  std::vector<char> data(origCap + 1, 'X');
  buf.push(data.data(), data.size());

  // 新容量 = 原容量*2 + len
  EXPECT_GE(buf.capacity(), origCap * 2);
}

TEST_F(BufferTest, ResizeAboveThreshold) {
  // 先扩容到超过阈值
  std::vector<char> initData(THRESHOLD_BUFFER_SIZE + 100, 'A');
  buf.push(initData.data(), initData.size());

  size_t capBeforeSecond = buf.capacity();

  // 再次扩容，超过阈值后按增量扩容
  std::vector<char> moreData(buf.writeAbleSize() + 100, 'B');
  buf.push(moreData.data(), moreData.size());

  // 新容量 = 原容量 + INCREMENT + len
  EXPECT_GE(buf.capacity(), capBeforeSecond + INCREMENT_BUFFER_SIZE);
}

TEST_F(BufferTest, MultipleResizes) {
  for (int i = 0; i < 10; i++) {
    std::vector<char> data(buf.writeAbleSize() + 1, static_cast<char>('A' + i));
    buf.push(data.data(), data.size());
  }

  EXPECT_GT(buf.capacity(), DEFAULT_BUFFER_SIZE);
  EXPECT_GT(buf.readAbleSize(), 0u);
}

// ===================== Capacity测试 =====================

TEST_F(BufferTest, CapacityInitial) {
  EXPECT_EQ(buf.capacity(), DEFAULT_BUFFER_SIZE);
}

TEST_F(BufferTest, CapacityAfterPush) {
  buf.push("hello", 5);
  EXPECT_EQ(buf.capacity(), DEFAULT_BUFFER_SIZE); // 不扩容
}

TEST_F(BufferTest, CapacityAfterResize) {
  std::vector<char> largeData(DEFAULT_BUFFER_SIZE + 1, 'X');
  buf.push(largeData.data(), largeData.size());
  EXPECT_GT(buf.capacity(), DEFAULT_BUFFER_SIZE);
}

TEST_F(BufferTest, CapacityUnchangedAfterReset) {
  std::vector<char> largeData(DEFAULT_BUFFER_SIZE + 1, 'X');
  buf.push(largeData.data(), largeData.size());
  size_t expandedCap = buf.capacity();

  buf.reset();

  EXPECT_EQ(buf.capacity(), expandedCap); // reset不缩减容量
}

// ===================== WriteAbleSize测试 =====================

TEST_F(BufferTest, WriteAbleSizeInitial) {
  EXPECT_EQ(buf.writeAbleSize(), DEFAULT_BUFFER_SIZE);
}

TEST_F(BufferTest, WriteAbleSizeAfterPush) {
  buf.push("hello", 5);
  EXPECT_EQ(buf.writeAbleSize(), DEFAULT_BUFFER_SIZE - 5);
}

TEST_F(BufferTest, WriteAbleSizeAfterReset) {
  buf.push("hello", 5);
  buf.reset();
  EXPECT_EQ(buf.writeAbleSize(), buf.capacity());
}

// ===================== 边界条件测试 =====================

TEST_F(BufferTest, PushExactCapacity) {
  std::vector<char> data(DEFAULT_BUFFER_SIZE, 'X');
  buf.push(data.data(), data.size());

  EXPECT_EQ(buf.readAbleSize(), DEFAULT_BUFFER_SIZE);
  EXPECT_EQ(buf.writeAbleSize(), 0u);
}

TEST_F(BufferTest, BeginPointerConsistency) {
  buf.push("hello", 5);
  const char *p1 = buf.begin();

  buf.push(" world", 6);
  const char *p2 = buf.begin();

  // 不扩容时，begin指针不变
  EXPECT_EQ(p1, p2);
}

TEST_F(BufferTest, BeginPointerAfterMoveReader) {
  buf.push("hello world", 11);
  const char *p1 = buf.begin();

  buf.moveReader(6);
  const char *p2 = buf.begin();

  EXPECT_EQ(p2, p1 + 6);
}

// ===================== 压力测试 =====================

TEST_F(BufferTest, ManySmallPushes) {
  const int iterations = 10000;
  for (int i = 0; i < iterations; i++) {
    buf.push("x", 1);
  }

  EXPECT_EQ(buf.readAbleSize(), static_cast<size_t>(iterations));
}

TEST_F(BufferTest, PushReadPushCycle) {
  for (int cycle = 0; cycle < 100; cycle++) {
    buf.push("test", 4);
    buf.moveReader(4);
  }

  EXPECT_TRUE(buf.empty());
}

TEST_F(BufferTest, ResetCycle) {
  for (int cycle = 0; cycle < 100; cycle++) {
    buf.push("test data", 9);
    buf.reset();
  }

  EXPECT_TRUE(buf.empty());
}

TEST_F(BufferTest, DataIntegrityLarge) {
  std::vector<char> original(1024 * 512, '\0'); // 512KB
  for (size_t i = 0; i < original.size(); i++) {
    original[i] = static_cast<char>(i % 256);
  }

  buf.push(original.data(), original.size());

  EXPECT_EQ(buf.readAbleSize(), original.size());
  EXPECT_EQ(std::memcmp(buf.begin(), original.data(), original.size()), 0);
}

TEST_F(BufferTest, DataIntegrityAfterResize) {
  // 先填充一些数据
  buf.push("prefix_", 7);

  // 触发扩容
  std::vector<char> largeData(DEFAULT_BUFFER_SIZE, 'Y');
  buf.push(largeData.data(), largeData.size());

  // 验证prefix仍在
  EXPECT_EQ(std::memcmp(buf.begin(), "prefix_", 7), 0);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
