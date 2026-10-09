#include "runtime/logging.h"
#include <gtest/gtest.h>
namespace {
class MemorySink : public zlog::LogSink {
  public:
    void log(const char *data, size_t size) override { bytes.append(data, size); }
    std::string bytes;
};
TEST(LoggingTest, IndependentErrorReceiversKeepTheirOwnLevelsAndSinks) {
    auto first = std::make_shared<MemorySink>();
    auto second = std::make_shared<MemorySink>();
    auto warning = zhttp::detail::network_error_logger("warning", first);
    auto error_only = zhttp::detail::network_error_logger("error", second);
    auto failure = znet::make_error(znet::ErrorKind::io, std::errc::io_error, "session");
    warning(failure);
    error_only(failure);
    EXPECT_NE(first->bytes.find("session"), std::string::npos);
    EXPECT_TRUE(second->bytes.empty());
    auto off = zhttp::detail::network_error_logger("off", second);
    off(failure);
    first->bytes.clear();
    warning(failure);
    EXPECT_FALSE(first->bytes.empty());
    EXPECT_TRUE(second->bytes.empty());
}
}
int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
