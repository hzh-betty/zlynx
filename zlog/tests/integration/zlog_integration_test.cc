/**
 * @brief zlog集成测试
 * 测试同步/异步日志器的完整工作流程
 */
#include "logger.h"
#include "sink.h"
#include "zlog.h"
#include <atomic>
#include <chrono>
#include <dirent.h>
#include <fstream>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <regex>
#include <sstream>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using namespace zlog;

// C++11兼容的文件操作辅助函数
namespace {
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

class ZlogIntegrationTest : public ::testing::Test {
protected:
  void SetUp() override {
    testDir = "integration_test_logs";
    createDir(testDir);
  }

  void TearDown() override { removeDir(testDir); }

  std::string readFile(const std::string &path) {
    std::ifstream ifs(path.c_str());
    std::stringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
  }

  int countLines(const std::string &content) {
    int count = 0;
    for (size_t i = 0; i < content.size(); ++i) {
      if (content[i] == '\n')
        count++;
    }
    return count;
  }

  std::string testDir;
};

// ===================== 同步日志器集成测试 =====================

TEST_F(ZlogIntegrationTest, SyncLoggerEndToEnd) {
  std::string logFile = testDir + "/sync_e2e.log";

  LocalLoggerBuilder builder;
  builder.buildLoggerName("sync_test");
  builder.buildLoggerType(LoggerType::LOGGER_SYNC);
  builder.buildLoggerLevel(LogLevel::value::INFO);
  builder.buildLoggerFormatter("[%p] %m%n");
  builder.buildLoggerSink<FileSink>(logFile);

  Logger::ptr logger = builder.build();
  ASSERT_NE(logger.get(), static_cast<Logger *>(NULL));

  logger->logImpl(LogLevel::value::INFO, __FILE__, __LINE__, "info message");
  logger->logImpl(LogLevel::value::WARNING, __FILE__, __LINE__,
                  "warning message");
  logger->logImpl(LogLevel::value::ERROR, __FILE__, __LINE__, "error message");

  std::string content = readFile(logFile);
  EXPECT_THAT(content, ::testing::HasSubstr("[INFO] info message"));
  EXPECT_THAT(content, ::testing::HasSubstr("[WARNING] warning message"));
  EXPECT_THAT(content, ::testing::HasSubstr("[ERROR] error message"));
  EXPECT_EQ(countLines(content), 3);
}

TEST_F(ZlogIntegrationTest, SyncLoggerLevelFilter) {
  std::string logFile = testDir + "/sync_filter.log";

  LocalLoggerBuilder builder;
  builder.buildLoggerName("filter_test");
  builder.buildLoggerType(LoggerType::LOGGER_SYNC);
  builder.buildLoggerLevel(LogLevel::value::WARNING);
  builder.buildLoggerFormatter("%p: %m%n");
  builder.buildLoggerSink<FileSink>(logFile);

  Logger::ptr logger = builder.build();

  logger->logImpl(LogLevel::value::DEBUG, __FILE__, __LINE__, "debug");
  logger->logImpl(LogLevel::value::INFO, __FILE__, __LINE__, "info");
  logger->logImpl(LogLevel::value::WARNING, __FILE__, __LINE__, "warning");
  logger->logImpl(LogLevel::value::ERROR, __FILE__, __LINE__, "error");

  std::string content = readFile(logFile);
  EXPECT_THAT(content, ::testing::Not(::testing::HasSubstr("debug")));
  EXPECT_THAT(content, ::testing::Not(::testing::HasSubstr("info")));
  EXPECT_THAT(content, ::testing::HasSubstr("warning"));
  EXPECT_THAT(content, ::testing::HasSubstr("error"));
  EXPECT_EQ(countLines(content), 2);
}

TEST_F(ZlogIntegrationTest, SyncLoggerMultipleSinks) {
  std::string logFile1 = testDir + "/sync_multi1.log";
  std::string logFile2 = testDir + "/sync_multi2.log";

  LocalLoggerBuilder builder;
  builder.buildLoggerName("multi_sink");
  builder.buildLoggerType(LoggerType::LOGGER_SYNC);
  builder.buildLoggerLevel(LogLevel::value::DEBUG);
  builder.buildLoggerFormatter("%m%n");
  builder.buildLoggerSink<FileSink>(logFile1);
  builder.buildLoggerSink<FileSink>(logFile2);

  Logger::ptr logger = builder.build();

  logger->logImpl(LogLevel::value::INFO, __FILE__, __LINE__, "multi sink test");

  std::string content1 = readFile(logFile1);
  std::string content2 = readFile(logFile2);
  EXPECT_EQ(content1, content2);
  EXPECT_THAT(content1, ::testing::HasSubstr("multi sink test"));
}

// ===================== 异步日志器集成测试 =====================

TEST_F(ZlogIntegrationTest, AsyncLoggerEndToEnd) {
  std::string logFile = testDir + "/async_e2e.log";

  {
    LocalLoggerBuilder builder;
    builder.buildLoggerName("async_test");
    builder.buildLoggerType(LoggerType::LOGGER_ASYNC);
    builder.buildLoggerLevel(LogLevel::value::DEBUG);
    builder.buildLoggerFormatter("[%p] %m%n");
    builder.buildWaitTime(std::chrono::milliseconds(100));
    builder.buildLoggerSink<FileSink>(logFile);

    Logger::ptr logger = builder.build();
    ASSERT_NE(logger.get(), static_cast<Logger *>(NULL));

    logger->logImpl(LogLevel::value::INFO, __FILE__, __LINE__,
                    "async message 1");
    logger->logImpl(LogLevel::value::INFO, __FILE__, __LINE__,
                    "async message 2");
    logger->logImpl(LogLevel::value::INFO, __FILE__, __LINE__,
                    "async message 3");

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }

  std::string content = readFile(logFile);
  EXPECT_THAT(content, ::testing::HasSubstr("async message 1"));
  EXPECT_THAT(content, ::testing::HasSubstr("async message 2"));
  EXPECT_THAT(content, ::testing::HasSubstr("async message 3"));
}

TEST_F(ZlogIntegrationTest, AsyncLoggerUnsafeMode) {
  std::string logFile = testDir + "/async_unsafe.log";

  {
    LocalLoggerBuilder builder;
    builder.buildLoggerName("async_unsafe");
    builder.buildLoggerType(LoggerType::LOGGER_ASYNC);
    builder.buildEnalleUnSafe();
    builder.buildLoggerLevel(LogLevel::value::DEBUG);
    builder.buildLoggerFormatter("%m%n");
    builder.buildWaitTime(std::chrono::milliseconds(50));
    builder.buildLoggerSink<FileSink>(logFile);

    Logger::ptr logger = builder.build();

    for (int i = 0; i < 1000; i++) {
      logger->logImpl(LogLevel::value::INFO, __FILE__, __LINE__,
                      ("message " + std::to_string(i)).c_str());
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  std::string content = readFile(logFile);
  EXPECT_FALSE(content.empty());
}

TEST_F(ZlogIntegrationTest, AsyncLoggerSafeMode) {
  std::string logFile = testDir + "/async_safe.log";

  {
    LocalLoggerBuilder builder;
    builder.buildLoggerName("async_safe");
    builder.buildLoggerType(LoggerType::LOGGER_ASYNC);
    builder.buildLoggerLevel(LogLevel::value::DEBUG);
    builder.buildLoggerFormatter("%m%n");
    builder.buildWaitTime(std::chrono::milliseconds(50));
    builder.buildLoggerSink<FileSink>(logFile);

    Logger::ptr logger = builder.build();

    for (int i = 0; i < 100; i++) {
      logger->logImpl(LogLevel::value::INFO, __FILE__, __LINE__,
                      ("safe message " + std::to_string(i)).c_str());
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }

  std::string content = readFile(logFile);
  for (int i = 0; i < 100; i++) {
    EXPECT_THAT(content,
                ::testing::HasSubstr("safe message " + std::to_string(i)));
  }
}

// ===================== 多线程测试 =====================

TEST_F(ZlogIntegrationTest, MultithreadedSyncLogger) {
  std::string logFile = testDir + "/mt_sync.log";

  LocalLoggerBuilder builder;
  builder.buildLoggerName("mt_sync");
  builder.buildLoggerType(LoggerType::LOGGER_SYNC);
  builder.buildLoggerLevel(LogLevel::value::DEBUG);
  builder.buildLoggerFormatter("[%t] %m%n");
  builder.buildLoggerSink<FileSink>(logFile);

  Logger::ptr logger = builder.build();

  std::atomic<int> counter(0);
  std::vector<std::thread> threads;

  for (int t = 0; t < 4; t++) {
    threads.push_back(std::thread([&logger, &counter, t]() {
      for (int i = 0; i < 100; i++) {
        logger->logImpl(
            LogLevel::value::INFO, __FILE__, __LINE__,
            ("thread" + std::to_string(t) + "_msg" + std::to_string(i))
                .c_str());
        counter++;
      }
    }));
  }

  for (size_t i = 0; i < threads.size(); ++i) {
    threads[i].join();
  }

  std::string content = readFile(logFile);
  EXPECT_EQ(countLines(content), 400);
  EXPECT_EQ(counter.load(), 400);
}

TEST_F(ZlogIntegrationTest, MultithreadedAsyncLogger) {
  std::string logFile = testDir + "/mt_async.log";

  {
    LocalLoggerBuilder builder;
    builder.buildLoggerName("mt_async");
    builder.buildLoggerType(LoggerType::LOGGER_ASYNC);
    builder.buildLoggerLevel(LogLevel::value::DEBUG);
    builder.buildLoggerFormatter("[%t] %m%n");
    builder.buildWaitTime(std::chrono::milliseconds(50));
    builder.buildLoggerSink<FileSink>(logFile);

    Logger::ptr logger = builder.build();

    std::vector<std::thread> threads;

    for (int t = 0; t < 4; t++) {
      threads.push_back(std::thread([&logger, t]() {
        for (int i = 0; i < 100; i++) {
          logger->logImpl(
              LogLevel::value::INFO, __FILE__, __LINE__,
              ("async_t" + std::to_string(t) + "_m" + std::to_string(i))
                  .c_str());
        }
      }));
    }

    for (size_t i = 0; i < threads.size(); ++i) {
      threads[i].join();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  std::string content = readFile(logFile);
  EXPECT_GT(countLines(content), 0);
}

// ===================== LoggerManager测试 =====================

TEST_F(ZlogIntegrationTest, GlobalLoggerBuilder) {
  std::string logFile = testDir + "/global.log";

  GlobalLoggerBuilder builder;
  builder.buildLoggerName("global_test");
  builder.buildLoggerType(LoggerType::LOGGER_SYNC);
  builder.buildLoggerLevel(LogLevel::value::DEBUG);
  builder.buildLoggerFormatter("%m%n");
  builder.buildLoggerSink<FileSink>(logFile);

  Logger::ptr logger = builder.build();
  ASSERT_NE(logger.get(), static_cast<Logger *>(NULL));

  EXPECT_TRUE(LoggerManager::getInstance().hasLogger("global_test"));

  Logger::ptr retrieved = LoggerManager::getInstance().getLogger("global_test");
  EXPECT_EQ(retrieved, logger);

  logger->logImpl(LogLevel::value::INFO, __FILE__, __LINE__,
                  "global logger test");

  std::string content = readFile(logFile);
  EXPECT_THAT(content, ::testing::HasSubstr("global logger test"));
}

TEST_F(ZlogIntegrationTest, RootLogger) {
  Logger::ptr root = LoggerManager::getInstance().rootLogger();
  ASSERT_NE(root.get(), static_cast<Logger *>(NULL));
  EXPECT_EQ(root->getName(), "root");

  Logger::ptr root2 = zlog::rootLogger();
  EXPECT_EQ(root, root2);
}

TEST_F(ZlogIntegrationTest, GetLoggerByName) {
  std::string logFile = testDir + "/named.log";

  GlobalLoggerBuilder builder;
  builder.buildLoggerName("named_logger");
  builder.buildLoggerType(LoggerType::LOGGER_SYNC);
  builder.buildLoggerFormatter("%m%n");
  builder.buildLoggerSink<FileSink>(logFile);
  builder.build();

  Logger::ptr logger = zlog::getLogger("named_logger");
  ASSERT_NE(logger.get(), static_cast<Logger *>(NULL));
  EXPECT_EQ(logger->getName(), "named_logger");
}

TEST_F(ZlogIntegrationTest, GetNonExistentLogger) {
  Logger::ptr logger = zlog::getLogger("non_existent");
  EXPECT_EQ(logger.get(), static_cast<Logger *>(NULL));
}

// ===================== 滚动文件集成测试 =====================

TEST_F(ZlogIntegrationTest, RollingFileIntegration) {
  std::string basename = testDir + "/rolling";

  {
    LocalLoggerBuilder builder;
    builder.buildLoggerName("rolling_test");
    builder.buildLoggerType(LoggerType::LOGGER_SYNC);
    builder.buildLoggerLevel(LogLevel::value::DEBUG);
    builder.buildLoggerFormatter("%m%n");
    builder.buildLoggerSink<RollBySizeSink>(basename, 1024UL);

    Logger::ptr logger = builder.build();

    for (int i = 0; i < 100; i++) {
      std::string msg = "rolling log message number " + std::to_string(i) +
                        " with some extra content to make it longer";
      logger->logImpl(LogLevel::value::INFO, __FILE__, __LINE__, msg.c_str());
    }
  }

  std::vector<std::string> files = listDir(testDir);
  int fileCount = 0;
  for (size_t i = 0; i < files.size(); ++i) {
    if (files[i].find("rolling") != std::string::npos) {
      fileCount++;
    }
  }

  EXPECT_GT(fileCount, 1) << "Expected multiple rolling files";
}

// ===================== 格式化集成测试 =====================

TEST_F(ZlogIntegrationTest, CompleteFormatIntegration) {
  std::string logFile = testDir + "/format.log";

  LocalLoggerBuilder builder;
  builder.buildLoggerName("format_test");
  builder.buildLoggerType(LoggerType::LOGGER_SYNC);
  builder.buildLoggerLevel(LogLevel::value::DEBUG);
  builder.buildLoggerFormatter(
      "[%d{%Y-%m-%d %H:%M:%S}][%t][%c][%f:%l][%p]%T%m%n");
  builder.buildLoggerSink<FileSink>(logFile);

  Logger::ptr logger = builder.build();
  logger->logImpl(LogLevel::value::INFO, "test.cc", 100, "formatted message");

  std::string content = readFile(logFile);

  std::regex pattern(
      R"(\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\]\[[^\]]+\]\[format_test\]\[test\.cc:100\]\[INFO\]\s+formatted message\n)");
  EXPECT_TRUE(std::regex_match(content, pattern)) << "Got: " << content;
}

// ===================== 压力测试 =====================

TEST_F(ZlogIntegrationTest, StressTestSync) {
  std::string logFile = testDir + "/stress_sync.log";

  LocalLoggerBuilder builder;
  builder.buildLoggerName("stress_sync");
  builder.buildLoggerType(LoggerType::LOGGER_SYNC);
  builder.buildLoggerLevel(LogLevel::value::DEBUG);
  builder.buildLoggerFormatter("%m%n");
  builder.buildLoggerSink<FileSink>(logFile);

  Logger::ptr logger = builder.build();

  std::chrono::high_resolution_clock::time_point start =
      std::chrono::high_resolution_clock::now();

  const int count = 10000;
  for (int i = 0; i < count; i++) {
    logger->logImpl(LogLevel::value::INFO, __FILE__, __LINE__,
                    "stress test message");
  }

  std::chrono::high_resolution_clock::time_point end =
      std::chrono::high_resolution_clock::now();
  long long duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
          .count();

  std::string content = readFile(logFile);
  EXPECT_EQ(countLines(content), count);

  std::cout << "Sync stress test: " << count << " messages in " << duration
            << "ms" << std::endl;
}

TEST_F(ZlogIntegrationTest, StressTestAsync) {
  std::string logFile = testDir + "/stress_async.log";

  {
    LocalLoggerBuilder builder;
    builder.buildLoggerName("stress_async");
    builder.buildLoggerType(LoggerType::LOGGER_ASYNC);
    builder.buildLoggerLevel(LogLevel::value::DEBUG);
    builder.buildLoggerFormatter("%m%n");
    builder.buildWaitTime(std::chrono::milliseconds(50));
    builder.buildLoggerSink<FileSink>(logFile);

    Logger::ptr logger = builder.build();

    std::chrono::high_resolution_clock::time_point start =
        std::chrono::high_resolution_clock::now();

    const int count = 10000;
    for (int i = 0; i < count; i++) {
      logger->logImpl(LogLevel::value::INFO, __FILE__, __LINE__,
                      "async stress test");
    }

    std::chrono::high_resolution_clock::time_point end =
        std::chrono::high_resolution_clock::now();
    long long duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
            .count();

    std::cout << "Async stress test: " << count << " messages pushed in "
              << duration << "ms" << std::endl;

    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  std::string content = readFile(logFile);
  EXPECT_GT(countLines(content), 0);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
