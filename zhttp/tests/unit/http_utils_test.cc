#include "zhttp/internal/http_utils.h"
#include "zhttp/zhttp_logger.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>

namespace zhttp {
namespace {

class TempDir {
  public:
    TempDir() {
        char tmpl[] = "/tmp/zhttp-http-utils-XXXXXX";
        char *created = ::mkdtemp(tmpl);
        if (created != nullptr) {
            path_ = created;
        }
    }

    ~TempDir() {
        if (!path_.empty()) {
            // 测试目录只创建少量文件，按固定文件名清理即可。
            std::remove((path_ + "/data.bin").c_str());
            std::remove((path_ + "/counter.txt").c_str());
            std::remove((path_ + "/bad-int.txt").c_str());
            ::rmdir(path_.c_str());
        }
    }

    const std::string &path() const { return path_; }

  private:
    std::string path_;
};

TEST(HttpUtilsTest, TimerHelperProvidesExpectedTimeUtilities) {
    EXPECT_EQ(TimerHelper::format_http_date_gmt(0),
              "Thu, 01 Jan 1970 00:00:00 GMT");

    const auto before = TimerHelper::steady_now();
    const auto after = TimerHelper::steady_now();
    EXPECT_LE(before, after);

    EXPECT_EQ(TimerHelper::milliseconds(123).count(), 123);
    EXPECT_EQ(TimerHelper::seconds(7).count(), 7);
    EXPECT_EQ(TimerHelper::to_milliseconds(std::chrono::seconds(3)).count(),
              3000);
}

TEST(HttpUtilsTest, PathOperatorNormalizesMatchesAndMapsPaths) {
    EXPECT_EQ(PathOperator::normalize_prefix(""), "/");
    EXPECT_EQ(PathOperator::normalize_prefix("/"), "/");
    EXPECT_EQ(PathOperator::normalize_prefix("assets/"), "/assets");
    EXPECT_EQ(PathOperator::normalize_prefix("/assets///"), "/assets");

    EXPECT_TRUE(PathOperator::should_handle_path("/x", "/"));
    EXPECT_FALSE(PathOperator::should_handle_path("x", "/"));
    EXPECT_TRUE(PathOperator::should_handle_path("/assets", "/assets"));
    EXPECT_TRUE(
        PathOperator::should_handle_path("/assets/js/app.js", "/assets"));
    EXPECT_FALSE(
        PathOperator::should_handle_path("/assets2/app.js", "/assets"));

    EXPECT_EQ(
        PathOperator::map_to_relative_path("/assets/js/app.js", "/assets"),
        "/js/app.js");
    EXPECT_EQ(PathOperator::map_to_relative_path("/assets", "/assets"), "/");
    EXPECT_EQ(PathOperator::map_to_relative_path("/index.html", "/"),
              "/index.html");
}

TEST(HttpUtilsTest, PathOperatorSanitizesAndJoinsPaths) {
    std::string out;
    ASSERT_TRUE(PathOperator::sanitize_relative_path("/a//b/./c", out));
    EXPECT_EQ(out, "a/b/c");

    ASSERT_TRUE(PathOperator::sanitize_relative_path("a%2Fb", out));
    EXPECT_EQ(out, "a/b");

    EXPECT_FALSE(PathOperator::sanitize_relative_path("../secret", out));
    EXPECT_FALSE(PathOperator::sanitize_relative_path("%2e%2e/secret", out));
    EXPECT_FALSE(PathOperator::sanitize_relative_path("a/../b", out));

    EXPECT_EQ(PathOperator::join_path("/base", "x.txt"), "/base/x.txt");
    EXPECT_EQ(PathOperator::join_path("/base/", "x.txt"), "/base/x.txt");
    EXPECT_EQ(PathOperator::join_path("", "x.txt"), "x.txt");
    EXPECT_EQ(PathOperator::join_path("/base", ""), "/base");
}

TEST(HttpUtilsTest, FileOperatorHandlesReadWriteAndMetadata) {
    TempDir tmp;
    ASSERT_FALSE(tmp.path().empty());

    const std::string file_path = tmp.path() + "/data.bin";
    const std::string int_path = tmp.path() + "/counter.txt";
    const std::string bad_int_path = tmp.path() + "/bad-int.txt";

    const std::string binary_payload("hello\0world", 11);
    EXPECT_TRUE(FileOperator::write_file_binary(file_path, binary_payload));

    std::string content;
    ASSERT_TRUE(FileOperator::read_file(file_path, content));
    EXPECT_EQ(content, binary_payload);

    EXPECT_TRUE(FileOperator::is_regular_file(file_path));
    EXPECT_FALSE(FileOperator::is_regular_file(tmp.path()));
    EXPECT_FALSE(FileOperator::is_regular_file("/tmp/not-exists-zhttp-file"));

    EXPECT_TRUE(FileOperator::is_directory(tmp.path()));
    EXPECT_FALSE(FileOperator::is_directory(file_path));
    EXPECT_FALSE(FileOperator::is_directory("/tmp/not-exists-zhttp-dir"));

    EXPECT_EQ(FileOperator::detect_content_type("index.html"), "text/html");
    EXPECT_EQ(FileOperator::detect_content_type("font.WOFF2"), "font/woff2");
    EXPECT_EQ(FileOperator::detect_content_type("README"),
              "application/octet-stream");
    EXPECT_EQ(FileOperator::detect_content_type("trailingdot."),
              "application/octet-stream");

    std::string last_modified;
    ASSERT_TRUE(FileOperator::get_last_modified(file_path, last_modified));
    EXPECT_NE(last_modified.find("GMT"), std::string::npos);
    EXPECT_FALSE(
        FileOperator::get_last_modified("/tmp/missing-zhttp", last_modified));

    std::string etag;
    ASSERT_TRUE(FileOperator::get_etag(file_path, etag));
    EXPECT_EQ(etag.find("W/\""), 0u);
    EXPECT_FALSE(FileOperator::get_etag("/tmp/missing-zhttp", etag));

    EXPECT_TRUE(FileOperator::write_int_to_file(int_path, 42));
    int value = 0;
    ASSERT_TRUE(FileOperator::read_int_from_file(int_path, value));
    EXPECT_EQ(value, 42);

    std::ofstream bad_int_file(bad_int_path);
    bad_int_file << "not-an-int";
    bad_int_file.close();
    EXPECT_FALSE(FileOperator::read_int_from_file(bad_int_path, value));
    EXPECT_FALSE(
        FileOperator::read_int_from_file("/tmp/missing-zhttp-int", value));
}

} // namespace
} // namespace zhttp

int main(int argc, char **argv) {
    zhttp::init_logger();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
