/**
 * @file buf_test.cc
 * @brief Buffer 单元测试
 */

#include "buff.h"
#include "znet_logger.h"
#include <gtest/gtest.h>
#include <cstring>

using namespace znet;

class BufferTest : public ::testing::Test {
protected:
  void SetUp() override {
    // 初始化测试数据
  }

  void TearDown() override {
    // 清理
  }
};

// 测试构造函数
TEST_F(BufferTest, Constructor) {
  Buffer buf;
  EXPECT_EQ(buf.readable_bytes(), 0u);
  // kInitialSize = 1024, kCheapPrepend = 8
  EXPECT_EQ(buf.writable_bytes(), 1024u);
  EXPECT_EQ(buf.prependable_bytes(), 8u);
}

// 测试基本的写入和读取
TEST_F(BufferTest, AppendAndRead) {
  Buffer buf;
  const char *data = "Hello, World!";
  size_t len = strlen(data);
  
  buf.append(data, len);
  EXPECT_EQ(buf.readable_bytes(), len);
  EXPECT_EQ(buf.writable_bytes(), 1024u - len); // kInitialSize = 1024
  
  std::string result = buf.read_all_as_string();
  EXPECT_EQ(result, std::string(data));
  EXPECT_EQ(buf.readable_bytes(), 0u);
}

// 测试字符串写入
TEST_F(BufferTest, AppendString) {
  Buffer buf;
  std::string str = "Test String";
  buf.append(str);
  
  EXPECT_EQ(buf.readable_bytes(), str.size());
  EXPECT_EQ(std::string(buf.peek(), buf.readable_bytes()), str);
}

// 测试整型写入和读取
TEST_F(BufferTest, IntegerReadWrite) {
  Buffer buf;
  
  // 测试 int8
  buf.write_int8(127);
  EXPECT_EQ(buf.read_int8(), 127);
  
  // 测试 int16
  buf.write_int16(12345);
  EXPECT_EQ(buf.read_int16(), 12345);
  
  // 测试 int32
  buf.write_int32(123456789);
  EXPECT_EQ(buf.read_int32(), 123456789);
  
  // 测试 int64
  buf.write_int64(1234567890123456LL);
  EXPECT_EQ(buf.read_int64(), 1234567890123456LL);
}

// 测试 retrieve
TEST_F(BufferTest, Retrieve) {
  Buffer buf;
  buf.append("0123456789", 10);
  EXPECT_EQ(buf.readable_bytes(), 10u);
  
  buf.retrieve(5);
  EXPECT_EQ(buf.readable_bytes(), 5u);
  EXPECT_EQ(std::string(buf.peek(), 5), "56789");
  
  buf.retrieve_all();
  EXPECT_EQ(buf.readable_bytes(), 0u);
}

// 测试自动扩容
TEST_F(BufferTest, AutoGrow) {
  Buffer buf;
  size_t initial_size = buf.writable_bytes();
  
  // 写入大量数据触发扩容
  std::string large_data(initial_size * 2, 'X');
  buf.append(large_data);
  
  EXPECT_EQ(buf.readable_bytes(), large_data.size());
  EXPECT_GE(buf.capacity(), large_data.size() + 8u); // kCheapPrepend = 8
}

// 测试 find_crlf
TEST_F(BufferTest, FindCRLF) {
  Buffer buf;
  buf.append("Hello\r\nWorld", 12);
  
  const char *crlf = buf.find_crlf();
  ASSERT_NE(crlf, nullptr);
  EXPECT_EQ(crlf - buf.peek(), 5);
}

// 测试 find_eol
TEST_F(BufferTest, FindEOL) {
  Buffer buf;
  buf.append("Hello\nWorld", 11);
  
  const char *eol = buf.find_eol();
  ASSERT_NE(eol, nullptr);
  EXPECT_EQ(eol - buf.peek(), 5);
}

// 测试读取指定长度的字符串
TEST_F(BufferTest, ReadString) {
  Buffer buf;
  buf.append("0123456789", 10);
  
  std::string str = buf.read_string(5);
  EXPECT_EQ(str, "01234");
  EXPECT_EQ(buf.readable_bytes(), 5u);
}

// 测试边界情况：空缓冲区
TEST_F(BufferTest, EmptyBuffer) {
  Buffer buf;
  EXPECT_EQ(buf.read_all_as_string(), "");
  EXPECT_EQ(buf.find_crlf(), nullptr);
  EXPECT_EQ(buf.find_eol(), nullptr);
}

// 测试 swap
TEST_F(BufferTest, Swap) {
  Buffer buf1, buf2;
  buf1.append("Buffer1", 7);
  buf2.append("Buffer2", 7);
  
  buf1.swap(buf2);
  
  EXPECT_EQ(buf1.read_all_as_string(), "Buffer2");
  EXPECT_EQ(buf2.read_all_as_string(), "Buffer1");
}

// 测试连续写入和读取
TEST_F(BufferTest, ContinuousReadWrite) {
  Buffer buf;
  
  for (int i = 0; i < 100; ++i) {
    buf.write_int32(i);
  }
  
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(buf.read_int32(), i);
  }
  
  EXPECT_EQ(buf.readable_bytes(), 0u);
}

int main(int argc, char **argv) {
  znet::init_logger(zlog::LogLevel::value::DEBUG);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
