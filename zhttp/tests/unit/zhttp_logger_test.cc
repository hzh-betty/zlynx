#include "zhttp/zhttp_logger.h"

#include <gtest/gtest.h>

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

bool file_exists(const std::string &path) {
    return ::access(path.c_str(), F_OK) == 0;
}

} // namespace

TEST(ZhttpLoggerTest, LazyGetLoggerInitializesDefaultLogger) {
    zlog::Logger *logger = zhttp::get_logger();
    ASSERT_NE(logger, nullptr);
    EXPECT_EQ(logger->get_name(), "zhttp_logger");
}

TEST(ZhttpLoggerTest, InitLoggerCoversSinkVariantsAndLegacyOverload) {
    (void)::mkdir("./logfile", 0755);
    const std::string default_file = "./logfile/zhttp.log";
    ::unlink(default_file.c_str());

    struct Case {
        std::string sink;
        bool provide_file_path;
        bool use_default_file;
    };

    const std::vector<Case> cases = {
        {"stdout", false, false},     {"file", false, true},
        {"both", true, false},        {"stdout+file", true, false},
        {"file+stdout", true, false}, {"unknown-sink", false, false},
    };

    for (size_t i = 0; i < cases.size(); ++i) {
        zhttp::LoggerInitOptions options;
        options.level = zlog::LogLevel::value::INFO;
        options.async = false;
        options.formatter.clear();
        options.sink = cases[i].sink;
        const std::string custom_file =
            "/tmp/zhttp_logger_case_" + std::to_string(i) + ".log";
        if (cases[i].provide_file_path) {
            options.file_path = custom_file;
            ::unlink(custom_file.c_str());
        }

        zhttp::init_logger(options);
        zlog::Logger *logger = zhttp::get_logger();
        ASSERT_NE(logger, nullptr);
        EXPECT_EQ(logger->get_name(), "zhttp_logger");
        EXPECT_EQ(dynamic_cast<zlog::AsyncLogger *>(logger), nullptr);

        logger->info(__FILE__, __LINE__, "zhttp logger sink case {}",
                     cases[i].sink);

        if (cases[i].use_default_file) {
            EXPECT_TRUE(file_exists(default_file));
        }
        if (cases[i].provide_file_path) {
            EXPECT_TRUE(file_exists(custom_file));
            ::unlink(custom_file.c_str());
        }
    }

    zhttp::init_logger(zlog::LogLevel::value::ERROR);
    zlog::Logger *legacy_logger = zhttp::get_logger();
    ASSERT_NE(legacy_logger, nullptr);
    EXPECT_EQ(legacy_logger->get_name(), "zhttp_logger");
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
