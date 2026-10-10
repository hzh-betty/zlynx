#include "zlog/sink.h"
#include "zlog/logger.h"
#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <thread>

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

class BufferedSinkTest : public SinkTest, public ::testing::WithParamInterface<FileBufferMode> {};

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

TEST_F(SinkTest, FileSinkBasic) {
    std::string filepath = testDir + "/test.log";

    {
        FileSink sink(filepath);
        sink.log("hello file\n", 11);
    }

    std::string content = readFile(filepath);
    EXPECT_EQ(content, "hello file\n");
}

TEST_P(BufferedSinkTest, FileSinkAppend) {
    std::string filepath = testDir + "/append.log";

    {
        FileSink sink(filepath, false, GetParam());
        sink.log("line1\n", 6);
    }

    {
        FileSink sink(filepath, false, GetParam());
        sink.log("line2\n", 6);
    }

    std::string content = readFile(filepath);
    EXPECT_EQ(content, "line1\nline2\n");
}

TEST_P(BufferedSinkTest, FileSinkLargeWrite) {
    std::string filepath = testDir + "/large.log";
    std::string largeData(1024 * 100, 'X'); // 100KB

    {
        FileSink sink(filepath, false, GetParam());
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

TEST_P(BufferedSinkTest, FileSinkAutoFlushWritesImmediately) {
    std::string filepath = testDir + "/autoflush.log";

    FileSink sink(filepath, true, GetParam());
    sink.log("flush-now", 9);

    std::string content = readFile(filepath);
    EXPECT_EQ(content, "flush-now");
}

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

TEST_P(BufferedSinkTest, RollBySizeSinkAutoFlushWritesImmediately) {
    std::string basename = testDir + "/roll_autoflush";
    RollBySizeSink sink(basename, 4096, true, 10, GetParam());

    sink.log("line-a\n", 7);
    sink.log("line-b\n", 7);

    std::vector<std::string> files = listDir(testDir);
    bool found = false;
    for (size_t i = 0; i < files.size(); ++i) {
        if (files[i].find("roll_autoflush") != std::string::npos) {
            found = true;
            std::string content = readFile(testDir + "/" + files[i]);
            EXPECT_THAT(content, ::testing::HasSubstr("line-a\n"));
            EXPECT_THAT(content, ::testing::HasSubstr("line-b\n"));
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(SinkTest, CreateStdOut) {
    LogSink::ptr sink = std::make_shared<StdOutSink>();
    EXPECT_NE(sink.get(), static_cast<LogSink *>(NULL));

    testing::internal::CaptureStdout();
    sink->log("factory test\n", 13);
    std::string output = testing::internal::GetCapturedStdout();

    EXPECT_EQ(output, "factory test\n");
}

TEST_F(SinkTest, CreateFileSink) {
    std::string filepath = testDir + "/factory_file.log";
    LogSink::ptr sink = std::make_shared<FileSink>(filepath);
    EXPECT_NE(sink.get(), static_cast<LogSink *>(NULL));

    sink->log("factory file test\n", 18);

    std::string content = readFile(filepath);
    EXPECT_EQ(content, "factory file test\n");
}

TEST_F(SinkTest, CreateRollBySizeSink) {
    std::string basename = testDir + "/factory_roll";
    LogSink::ptr sink = std::make_shared<RollBySizeSink>(basename, 1024UL);
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

TEST_F(SinkTest, FileFailuresAreReportedDuringOpenWriteAndFlush) {
    const std::string blocked = testDir + "/not_a_directory";
    { std::ofstream file(blocked); file << "existing file"; }
    EXPECT_THROW(FileSink(blocked + "/log.txt"), std::system_error);
    EXPECT_THROW(RollBySizeSink(blocked + "/roll", 100), std::system_error);
    FileSink full("/dev/full", true);
    EXPECT_THROW(full.log("hello", 5), std::ios_base::failure);
    EXPECT_THROW(full.flush(), std::ios_base::failure);
}

TEST_P(BufferedSinkTest, OversizedRollingRecordIsRejectedWithoutRotationOrDataLoss) {
    RollBySizeSink sink(testDir + "/limit", 10, false, 10, GetParam());
    sink.log("before", 6);
    EXPECT_THROW(sink.log("01234567890", 11), std::length_error);
    sink.log("next", 4);
    sink.flush();
    const auto files = listDir(testDir);
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(readFile(testDir + "/" + files.front()), "beforenext");
    EXPECT_THROW(RollBySizeSink(testDir + "/zero", 0), std::invalid_argument);
    EXPECT_THROW(RollBySizeSink(testDir + "/zero", 10, false, 0), std::invalid_argument);
}

TEST_P(BufferedSinkTest, RecreatingRollingSinkDoesNotReuseExistingFile) {
    { RollBySizeSink sink(testDir + "/reopen", 10, false, 10, GetParam()); sink.log("AAAAAAAA", 8); }
    { RollBySizeSink sink(testDir + "/reopen", 10, false, 10, GetParam()); sink.log("BBBBBBBB", 8); }
    const auto files = listDir(testDir);
    ASSERT_EQ(files.size(), 2u);
    std::vector<std::string> contents;
    for (const auto &file : files) contents.push_back(readFile(testDir + "/" + file));
    std::sort(contents.begin(), contents.end());
    EXPECT_EQ(contents, (std::vector<std::string>{"AAAAAAAA", "BBBBBBBB"}));
}

TEST_P(BufferedSinkTest, RollingRetentionIncludesFilesFromPreviousInstances) {
    // 保留上限跨重建仍生效，不能只计当前进程创建的文件。
    for (int i = 0; i < 15; ++i) {
        RollBySizeSink sink(testDir + "/retained", 10, false, 10, GetParam());
        const std::string message = std::to_string(i);
        sink.log(message.data(), message.size());
    }
    EXPECT_EQ(listDir(testDir).size(), 10u);
    { RollBySizeSink sink(testDir + "/retained", 10, false, 3, GetParam()); sink.log("last", 4); }
    const auto files = listDir(testDir);
    ASSERT_EQ(files.size(), 3u);
    std::vector<std::string> contents;
    for (const auto &file : files) contents.push_back(readFile(testDir + "/" + file));
    std::sort(contents.begin(), contents.end());
    EXPECT_EQ(contents, (std::vector<std::string>{"13", "14", "last"}));
}

TEST_P(BufferedSinkTest, AsyncRollingKeepsSmallRecordsWithinFileLimit) {
    const auto sink = std::make_shared<RollBySizeSink>(testDir + "/async_roll", 100, false, 10, GetParam());
    AsyncLogger logger("async_roll", LogLevel::value::INFO, std::make_shared<Formatter>("%m"),
                       {sink}, AsyncType::ASYNC_SAFE, std::chrono::hours(1));
    for (int i = 0; i < 100; ++i) logger.info(__FILE__, __LINE__, "0123456789");
    logger.close();
    const auto files = listDir(testDir);
    ASSERT_EQ(files.size(), 10u);
    size_t total = 0;
    for (const auto &file : files) {
        const auto content = readFile(testDir + "/" + file);
        EXPECT_EQ(content.size(), 100u);
        total += content.size();
    }
    EXPECT_EQ(total, 1000u);
}

TEST_P(BufferedSinkTest, SharedFileSinkWorksAcrossSyncAndAsyncLoggers) {
    const std::string path = testDir + "/shared.log";
    const auto sink = std::make_shared<FileSink>(path, false, GetParam());
    const auto formatter = std::make_shared<Formatter>("%m%n");
    const std::vector<Logger::ptr> loggers{
        std::make_shared<SyncLogger>("first", LogLevel::value::INFO, formatter, std::vector<LogSink::ptr>{sink}),
        std::make_shared<SyncLogger>("second", LogLevel::value::INFO, formatter, std::vector<LogSink::ptr>{sink}),
        std::make_shared<AsyncLogger>("third", LogLevel::value::INFO, formatter, std::vector<LogSink::ptr>{sink},
                                     AsyncType::ASYNC_SAFE, std::chrono::hours(1))};
    std::vector<std::thread> writers;
    for (size_t t = 0; t < loggers.size(); ++t) {
        writers.emplace_back([&, t] {
            for (int i = 0; i < 200; ++i) loggers[t]->info(__FILE__, __LINE__, "{}:{}", t, i);
        });
    }
    for (auto &writer : writers) writer.join();
    for (const auto &logger : loggers) logger->close();
    std::istringstream content(readFile(path));
    std::vector<std::string> actual, expected;
    for (std::string line; std::getline(content, line);) actual.push_back(line);
    for (int t = 0; t < 3; ++t) {
        for (int i = 0; i < 200; ++i) expected.push_back(std::to_string(t) + ":" + std::to_string(i));
    }
    std::sort(actual.begin(), actual.end());
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(actual, expected);
}

TEST_P(BufferedSinkTest, SharedRollingSinkSerializesWritesAndRotation) {
    const auto sink = std::make_shared<RollBySizeSink>(testDir + "/shared_roll", 100, false, 100, GetParam());
    const auto formatter = std::make_shared<Formatter>("%m");
    SyncLogger first("first", LogLevel::value::INFO, formatter, {sink});
    SyncLogger second("second", LogLevel::value::INFO, formatter, {sink});
    std::thread a([&] { for (int i = 0; i < 100; ++i) first.info(__FILE__, __LINE__, "AAAAAAAAAA"); });
    std::thread b([&] { for (int i = 0; i < 100; ++i) second.info(__FILE__, __LINE__, "BBBBBBBBBB"); });
    a.join(); b.join();
    first.close();
    // 关闭一个 logger 不能关闭另一个 logger 共用的 sink。
    second.info(__FILE__, __LINE__, "CCCCCCCCCC");
    second.close();
    size_t total = 0, a_count = 0, b_count = 0, c_count = 0;
    for (const auto &file : listDir(testDir)) {
        const auto content = readFile(testDir + "/" + file);
        EXPECT_LE(content.size(), 100u);
        total += content.size();
        a_count += std::count(content.begin(), content.end(), 'A');
        b_count += std::count(content.begin(), content.end(), 'B');
        c_count += std::count(content.begin(), content.end(), 'C');
    }
    EXPECT_EQ(total, 2010u);
    EXPECT_EQ(a_count, 1000u);
    EXPECT_EQ(b_count, 1000u);
    EXPECT_EQ(c_count, 10u);
}

INSTANTIATE_TEST_SUITE_P(FileBufferModes, BufferedSinkTest,
    ::testing::Values(FileBufferMode::UNBUFFERED, FileBufferMode::BUFFERED));

TEST_F(SinkTest, BufferedFileFlushPreservesBinaryRecords) {
    const std::string path = testDir + "/buffered.log";
    FileSink sink(path, false, FileBufferMode::BUFFERED);
    const char bytes[] = {'a', '\0', 'b', '\n'};
    for (int i = 0; i < 100; ++i) sink.log(bytes, sizeof bytes);
    sink.flush();
    std::string expected;
    for (int i = 0; i < 100; ++i) expected.append(bytes, sizeof bytes);
    EXPECT_EQ(readFile(path), expected);
}

TEST_F(SinkTest, BufferedWriteErrorsAreReportedOnFlushAndAutoFlush) {
    FileSink buffered("/dev/full", false, FileBufferMode::BUFFERED);
    EXPECT_NO_THROW(buffered.log("hello", 5));
    EXPECT_THROW(buffered.flush(), std::ios_base::failure);
    FileSink immediate("/dev/full", true, FileBufferMode::BUFFERED);
    EXPECT_THROW(immediate.log("hello", 5), std::ios_base::failure);
}

TEST_F(SinkTest, BufferedFailuresStillFlushOtherSinksAndCompleteClose) {
    class RecordingSink : public LogSink {
      public:
        std::string text;
        int flushes = 0;
        void log(const char *data, size_t len) override { text.append(data, len); }
        void flush() override { ++flushes; }
    };
    for (bool async : {false, true}) {
        const auto good = std::make_shared<RecordingSink>();
        const auto bad = std::make_shared<FileSink>("/dev/full", false, FileBufferMode::BUFFERED);
        const std::vector<LogSink::ptr> sinks{bad, good};
        const auto format = std::make_shared<Formatter>("%m%n");
        Logger::ptr logger;
        if (async) {
            logger = std::make_shared<AsyncLogger>("buffered", LogLevel::value::INFO,
                format, sinks, AsyncType::ASYNC_SAFE, std::chrono::hours(1));
        } else {
            logger = std::make_shared<SyncLogger>("buffered", LogLevel::value::INFO, format, sinks);
        }
        logger->info(__FILE__, __LINE__, "first");
        logger->info(__FILE__, __LINE__, "second");
        EXPECT_THROW(logger->flush(), std::ios_base::failure);
        EXPECT_EQ(good->text, "first\nsecond\n");
        EXPECT_EQ(good->flushes, 1);
        EXPECT_THROW(logger->close(), std::ios_base::failure);
        EXPECT_EQ(good->flushes, 2);
        EXPECT_NO_THROW(logger->close());
        EXPECT_THROW(logger->info(__FILE__, __LINE__, "closed"), std::runtime_error);
    }
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
