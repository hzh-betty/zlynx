#include "zlog/logger.h"
#include "zlog/logger_builder.h"
#include "zlog/message.h"

#include <gtest/gtest.h>

#include <dirent.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

namespace zlog {
namespace {

class RecordingSink : public LogSink {
  public:
    void log(const char *data, size_t len) override {
        output.append(data, len);
    }

    std::string output;
};

class StringViewTest : public ::testing::Test {
  protected:
    void SetUp() override {
        char path[] = "/tmp/zlog_string_view_XXXXXX";
        const char *directory = mkdtemp(path);
        ASSERT_NE(directory, nullptr);
        directory_ = directory;
    }

    void TearDown() override {
        DIR *directory = opendir(directory_.c_str());
        if (directory) {
            while (const dirent *entry = readdir(directory)) {
                const std::string name = entry->d_name;
                if (name != "." && name != "..") {
                    unlink((directory_ + "/" + name).c_str());
                }
            }
            closedir(directory);
        }
        rmdir(directory_.c_str());
    }

    std::string read_file(const std::string &path) {
        std::ifstream stream(path.c_str(), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream),
                           std::istreambuf_iterator<char>());
    }

    std::string directory_;
};

TEST_F(StringViewTest, FormatterPreservesPayloadAndNameLengths) {
    const char payload[] = {'a', '\0', 'b'};
    const char name[] = {'n', '\0', 'm'};
    const LogMessage message(LogLevel::value::INFO, __FILE__, __LINE__,
                             fmt::string_view(payload, sizeof(payload)),
                             fmt::string_view(name, sizeof(name)));
    const Formatter formatter("%c:%m");
    fmt::memory_buffer output;
    formatter.format(output, message);
    EXPECT_EQ(std::string(output.data(), output.size()),
              std::string(name, sizeof(name)) + ":" +
                  std::string(payload, sizeof(payload)));
}

TEST_F(StringViewTest, EmptyViewsAndLegacyNullPointersRemainValid) {
    const Formatter formatter("%c%m");
    fmt::memory_buffer output;
    const LogMessage empty(LogLevel::value::INFO, nullptr, 0,
                           fmt::string_view(), fmt::string_view());
    formatter.format(output, empty);
    EXPECT_EQ(output.size(), 0u);
    const LogMessage legacy(LogLevel::value::INFO, nullptr, 0, nullptr,
                            nullptr);
    formatter.format(output, legacy);
    EXPECT_EQ(output.size(), 0u);
}

TEST_F(StringViewTest, BuilderCopiesAnUnterminatedNameSlice) {
    LoggerBuilder builder;
    {
        const char name[] = {'x', 'n', 'a', 'm', 'e', 'x'};
        builder.build_logger_name(fmt::string_view(name + 1, 4));
    }
    const Logger::ptr logger = builder.build();
    ASSERT_NE(logger, nullptr);
    EXPECT_EQ(logger->get_name(), "name");
}

TEST_F(StringViewTest, SyncAndAsyncLoggingPreserveBinaryPayloadAndOwnedName) {
    const char payload[] = {'a', '\0', 'b'};
    const char format[] = {'{', '}'};
    const std::string name("n\0m", 3);
    const std::shared_ptr<RecordingSink> sink =
        std::make_shared<RecordingSink>();
    const std::vector<LogSink::ptr> sinks{sink};
    const Formatter::ptr formatter = std::make_shared<Formatter>("%c:%m");
    {
        SyncLogger logger(name, LogLevel::value::INFO, formatter, sinks);
        logger.info(__FILE__, __LINE__,
                    fmt::string_view(format, sizeof(format)),
                    fmt::string_view(payload, sizeof(payload)));
    }
    {
        AsyncLogger logger(name, LogLevel::value::INFO, formatter, sinks,
                           AsyncType::ASYNC_SAFE, std::chrono::hours(1));
        logger.info(__FILE__, __LINE__, "{}",
                    fmt::string_view(payload, sizeof(payload)));
    }
    const std::string expected =
        name + ":" + std::string(payload, sizeof(payload));
    EXPECT_EQ(sink->output, expected + expected);
}

TEST_F(StringViewTest, StdoutPreservesUnterminatedBytesAndEmbeddedNul) {
    const char text[] = {'a', 'b', 'c'};
    const char binary[] = {'a', '\0', 'b'};
    StdOutSink sink;
    ::testing::internal::CaptureStdout();
    sink.log(text, sizeof(text));
    sink.log(binary, sizeof(binary));
    EXPECT_EQ(::testing::internal::GetCapturedStdout(),
              std::string(text, sizeof(text)) +
                  std::string(binary, sizeof(binary)));
}

TEST_F(StringViewTest, FilePreservesUnterminatedBytesAndEmbeddedNul) {
    const char text[] = {'a', 'b', 'c'};
    const char binary[] = {'a', '\0', 'b'};
    const std::string path = directory_ + "/file.log";
    {
        FileSink sink(path);
        sink.log(text, sizeof(text));
        sink.log(binary, sizeof(binary));
    }
    EXPECT_EQ(read_file(path), std::string(text, sizeof(text)) +
                                   std::string(binary, sizeof(binary)));
}

TEST_F(StringViewTest, RollingFilePreservesBytesAcrossRotation) {
    const char first[] = {'a', '\0', 'b'};
    const char second[] = {'x', 'y', 'z'};
    {
        RollBySizeSink sink(directory_ + "/roll", sizeof(first));
        sink.log(first, sizeof(first));
        sink.log(second, sizeof(second));
    }
    size_t files = 0;
    DIR *directory = opendir(directory_.c_str());
    ASSERT_NE(directory, nullptr);
    while (const dirent *entry = readdir(directory)) {
        const std::string name = entry->d_name;
        if (name != "." && name != "..") {
            ++files;
            const std::string expected =
                name.find("-0.log") != std::string::npos
                    ? std::string(first, sizeof(first))
                    : std::string(second, sizeof(second));
            EXPECT_EQ(read_file(directory_ + "/" + name), expected);
        }
    }
    closedir(directory);
    EXPECT_EQ(files, 2u);
}

} // namespace
} // namespace zlog

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
