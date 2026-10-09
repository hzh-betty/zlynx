#include "async/buffer.h"
#include "zlog/logger.h"
#include "zlog/logger_builder.h"

#include <gtest/gtest.h>

#include <string>
#include <type_traits>

namespace zlog {
namespace {

using detail::Buffer;

class RecordingSink : public LogSink {
  public:
    void log(const char *data, size_t len) override {
        output.append(data, len);
    }

    std::string output;
};

TEST(OwnershipTest, SyncLoggerCopiesNameAndUsesItInPattern) {
    std::string name(128, 'a');
    const std::string expected_name = name;
    const auto formatter = std::make_shared<Formatter>("%c %m%n");
    const auto sink = std::make_shared<RecordingSink>();
    std::vector<LogSink::ptr> sinks{sink};
    SyncLogger logger(name.c_str(), LogLevel::value::DEBUG, formatter, sinks);

    // 修改调用方的存储，日志器名称和格式化结果应仍使用构造时的副本。
    name.assign(name.size(), 'b');
    EXPECT_EQ(logger.get_name(), expected_name);
    logger.info(__FILE__, __LINE__, "message");
    EXPECT_EQ(sink->output, expected_name + " message\n");
}

TEST(OwnershipTest, AsyncLoggerOwnsNameAndDrainsOnDestruction) {
    const std::string expected_name(128, 'a');
    const auto sink = std::make_shared<RecordingSink>();
    const auto formatter = std::make_shared<Formatter>("%c %m%n");
    std::vector<LogSink::ptr> sinks{sink};
    Logger::ptr logger;
    {
        std::string name = expected_name;
        logger = std::make_shared<AsyncLogger>(
            name.c_str(), LogLevel::value::DEBUG, formatter, sinks,
            AsyncType::ASYNC_SAFE, std::chrono::hours(1));
    }

    EXPECT_EQ(logger->get_name(), expected_name);
    logger->info(__FILE__, __LINE__, "pending");
    // 小消息不会达到消费阈值；通过析构等待排空，不依赖固定睡眠。
    sinks.clear();
    logger.reset();
    EXPECT_EQ(sink->output, expected_name + " pending\n");
}

TEST(OwnershipTest, BuilderCopiesNameBeforeBuild) {
    LoggerBuilder builder;
    const std::string expected_name(128, 'a');
    {
        std::string name = expected_name;
        builder.build_logger_name(name.c_str());
        name.assign(name.size(), 'b');
    }

    const auto logger = builder.build();
    ASSERT_NE(logger, nullptr);
    EXPECT_EQ(logger->get_name(), expected_name);
}

TEST(OwnershipTest, AsyncLoggerDrainsBeforeDestroyingItsOnlySink) {
    std::string output;
    bool destroyed = false;

    class LifetimeSink : public LogSink {
      public:
        LifetimeSink(std::string &output, bool &destroyed)
            : output_(output), destroyed_(destroyed) {}

        ~LifetimeSink() override { destroyed_ = true; }

        void log(const char *data, size_t len) override {
            output_.append(data, len);
        }

      private:
        std::string &output_;
        bool &destroyed_;
    };

    std::vector<LogSink::ptr> sinks{
        std::make_shared<LifetimeSink>(output, destroyed)};
    const std::weak_ptr<LogSink> sink = sinks.front();
    const auto formatter = std::make_shared<Formatter>("%m%n");
    auto logger = std::make_shared<AsyncLogger>(
        "sink_lifetime", LogLevel::value::DEBUG, formatter, sinks,
        AsyncType::ASYNC_SAFE, std::chrono::hours(1));
    // 释放调用方的引用，让 sink 仅由日志器持有。
    sinks.clear();
    logger->info(__FILE__, __LINE__, "pending");
    EXPECT_FALSE(destroyed);
    logger.reset();
    EXPECT_TRUE(destroyed);
    EXPECT_TRUE(sink.expired());
    EXPECT_EQ(output, "pending\n");
}

TEST(OwnershipTest, BuiltLoggerKeepsNameAfterBuilderReuseAndDestruction) {
    const std::string expected_name(128, 'a');
    Logger::ptr logger;
    {
        LoggerBuilder builder;
        std::string name = expected_name;
        builder.build_logger_name(name.c_str());
        logger = builder.build();
        builder.build_logger_name("other");
        const auto other = builder.build();
        ASSERT_NE(other, nullptr);
        EXPECT_EQ(other->get_name(), "other");
    }

    ASSERT_NE(logger, nullptr);
    EXPECT_EQ(logger->get_name(), expected_name);
}

TEST(OwnershipTest, EmptyAndUnsetBuilderNamesRemainDistinct) {
    LoggerBuilder builder;
    EXPECT_EQ(builder.build(), nullptr);
    builder.build_logger_name("");
    const auto logger = builder.build();
    ASSERT_NE(logger, nullptr);
    EXPECT_EQ(logger->get_name(), "");
    builder.build_logger_name(nullptr);
    EXPECT_EQ(builder.build(), nullptr);
}

TEST(OwnershipTest, BufferCannotShareItsAllocationThroughCopyOrMove) {
    EXPECT_FALSE(std::is_copy_constructible<Buffer>::value);
    EXPECT_FALSE(std::is_copy_assignable<Buffer>::value);
    EXPECT_FALSE(std::is_move_constructible<Buffer>::value);
    EXPECT_FALSE(std::is_move_assignable<Buffer>::value);
}

} // namespace
} // namespace zlog

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
