#include "sink.h"
#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

using namespace zlog;

namespace {
bool dirExists(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool fileExists(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

void createDir(const std::string &path) { mkdir(path.c_str(), 0755); }

void removeDir(const std::string &path) {
  DIR *dir = opendir(path.c_str());
  if (dir) {
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
      std::string name = entry->d_name;
      if (name != "." && name != "..") {
        std::string fullPath = path + "/" + name;
        struct stat st;
        if (stat(fullPath.c_str(), &st) == 0) {
          if (S_ISDIR(st.st_mode)) {
            removeDir(fullPath);
          } else {
            unlink(fullPath.c_str());
          }
        }
      }
    }
    closedir(dir);
  }
  rmdir(path.c_str());
}

std::vector<std::string> listDir(const std::string &path) {
  std::vector<std::string> files;
  DIR *dir = opendir(path.c_str());
  if (dir) {
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
      std::string name = entry->d_name;
      if (name != "." && name != "..") {
        files.push_back(name);
      }
    }
    closedir(dir);
  }
  return files;
}
} // namespace

class SinkTest : public ::testing::Test {
protected:
  void SetUp() override {
    testDir = "test_logs";
    createDir(testDir);
  }

  void TearDown() override { removeDir(testDir); }

  std::string readFile(const std::string &path) {
    std::ifstream ifs(path.c_str());
    std::stringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
  }

  std::string testDir;
};

// ===================== StdOutSink Tests =====================

TEST_F(SinkTest, StdOutSinkBasic) {
  StdOutSink sink;

  testing::internal::CaptureStdout();
  sink.log("hello stdout\n", 13);
  std::string output = testing::internal::GetCapturedStdout();

  EXPECT_EQ(output, "hello stdout\n");
}

TEST_F(SinkTest, StdOutSinkMultipleWrites) {
  StdOutSink sink;

  testing::internal::CaptureStdout();
  sink.log("line1\n", 6);
  sink.log("line2\n", 6);
  sink.log("line3\n", 6);
  std::string output = testing::internal::GetCapturedStdout();

  EXPECT_EQ(output, "line1\nline2\nline3\n");
}

TEST_F(SinkTest, StdOutSinkEmptyString) {
  StdOutSink sink;

  testing::internal::CaptureStdout();
  sink.log("", 0);
  std::string output = testing::internal::GetCapturedStdout();

  EXPECT_EQ(output, "");
}

// ===================== FileSink Tests =====================

TEST_F(SinkTest, FileSinkBasic) {
  std::string filepath = testDir + "/test.log";

  {
    FileSink sink(filepath);
    sink.log("hello file\n", 11);
  }

  std::string content = readFile(filepath);
  EXPECT_EQ(content, "hello file\n");
}

TEST_F(SinkTest, FileSinkAppend) {
  std::string filepath = testDir + "/append.log";

  {
    FileSink sink(filepath);
    sink.log("line1\n", 6);
  }

  {
    FileSink sink(filepath);
    sink.log("line2\n", 6);
  }

  std::string content = readFile(filepath);
  EXPECT_EQ(content, "line1\nline2\n");
}

TEST_F(SinkTest, FileSinkLargeWrite) {
  std::string filepath = testDir + "/large.log";
  std::string largeData(1024 * 100, 'X'); // 100KB

  {
    FileSink sink(filepath);
    sink.log(largeData.c_str(), largeData.size());
  }

  std::string content = readFile(filepath);
  EXPECT_EQ(content.size(), largeData.size());
  EXPECT_EQ(content, largeData);
}

TEST_F(SinkTest, FileSinkMultipleWrites) {
  std::string filepath = testDir + "/multi.log";

  {
    FileSink sink(filepath);
    for (int i = 0; i < 100; i++) {
      std::string line = "log line " + std::to_string(i) + "\n";
      sink.log(line.c_str(), line.size());
    }
  }

  std::string content = readFile(filepath);
  for (int i = 0; i < 100; i++) {
    std::string expected = "log line " + std::to_string(i) + "\n";
    EXPECT_THAT(content, ::testing::HasSubstr(expected));
  }
}

// ===================== RollBySizeSink Tests =====================

TEST_F(SinkTest, RollBySizeSinkBasic) {
  std::string basename = testDir + "/roll_test";
  size_t maxSize = 1024; // 1KB

  {
    RollBySizeSink sink(basename, maxSize);
    sink.log("test message\n", 13);
  }

  // 检查是否创建了日志文件
  std::vector<std::string> files = listDir(testDir);
  bool found = false;
  for (size_t i = 0; i < files.size(); ++i) {
    if (files[i].find("roll_test") != std::string::npos) {
      found = true;
      std::string content = readFile(testDir + "/" + files[i]);
      EXPECT_EQ(content, "test message\n");
    }
  }
  EXPECT_TRUE(found) << "Roll log file not found";
}

TEST_F(SinkTest, RollBySizeSinkRollOver) {
  std::string basename = testDir + "/roll_over";
  size_t maxSize = 100; // 100字节，很小以便触发滚动

  {
    RollBySizeSink sink(basename, maxSize);

    for (int i = 0; i < 10; i++) {
      std::string line =
          "this is a longer log message line " + std::to_string(i) + "\n";
      sink.log(line.c_str(), line.size());
    }
  }

  // 应该创建了多个日志文件
  std::vector<std::string> files = listDir(testDir);
  int fileCount = 0;
  for (size_t i = 0; i < files.size(); ++i) {
    if (files[i].find("roll_over") != std::string::npos) {
      fileCount++;
    }
  }

  EXPECT_GT(fileCount, 1) << "Expected multiple rolled files";
}

TEST_F(SinkTest, RollBySizeSinkLargeFile) {
  std::string basename = testDir + "/large_roll";
  size_t maxSize = 1024 * 1024; // 1MB

  {
    RollBySizeSink sink(basename, maxSize);

    std::string chunk(10240, 'A'); // 10KB
    for (int i = 0; i < 50; i++) {
      sink.log(chunk.c_str(), chunk.size());
    }
  }

  // 500KB < 1MB，不应该滚动
  std::vector<std::string> files = listDir(testDir);
  int fileCount = 0;
  for (size_t i = 0; i < files.size(); ++i) {
    if (files[i].find("large_roll") != std::string::npos) {
      fileCount++;
    }
  }

  EXPECT_EQ(fileCount, 1);
}

// ===================== SinkFactory Tests =====================

TEST_F(SinkTest, SinkFactoryCreateStdOut) {
  LogSink::ptr sink = SinkFactory::create<StdOutSink>();
  EXPECT_NE(sink.get(), static_cast<LogSink *>(NULL));

  testing::internal::CaptureStdout();
  sink->log("factory test\n", 13);
  std::string output = testing::internal::GetCapturedStdout();

  EXPECT_EQ(output, "factory test\n");
}

TEST_F(SinkTest, SinkFactoryCreateFileSink) {
  std::string filepath = testDir + "/factory_file.log";
  LogSink::ptr sink = SinkFactory::create<FileSink>(filepath);
  EXPECT_NE(sink.get(), static_cast<LogSink *>(NULL));

  sink->log("factory file test\n", 18);

  std::string content = readFile(filepath);
  EXPECT_EQ(content, "factory file test\n");
}

TEST_F(SinkTest, SinkFactoryCreateRollBySizeSink) {
  std::string basename = testDir + "/factory_roll";
  LogSink::ptr sink = SinkFactory::create<RollBySizeSink>(basename, 1024UL);
  EXPECT_NE(sink.get(), static_cast<LogSink *>(NULL));

  sink->log("factory roll test\n", 18);

  std::vector<std::string> files = listDir(testDir);
  bool found = false;
  for (size_t i = 0; i < files.size(); ++i) {
    if (files[i].find("factory_roll") != std::string::npos) {
      found = true;
    }
  }
  EXPECT_TRUE(found);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
