#include "zhttp/http_common.h"
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
            ::rmdir(path_.c_str());
        }
    }

    const std::string &path() const { return path_; }

  private:
    std::string path_;
};

TEST(HttpUtilsTest, EtagUsesProvidedSizeAndNanosecondTimestamp) {
    struct stat info = {};
    info.st_size = 123;
    info.st_mtim.tv_sec = 1;
    info.st_mtim.tv_nsec = 7;
    EXPECT_EQ(file::get_etag(info), "W/\"123-1000000007\"");
}

TEST(HttpUtilsTest, StaticPathsNormalizeMatchAndMap) {
    EXPECT_EQ(static_path::normalize_prefix(""), "/");
    EXPECT_EQ(static_path::normalize_prefix("/"), "/");
    EXPECT_EQ(static_path::normalize_prefix("assets/"), "/assets");
    EXPECT_EQ(static_path::normalize_prefix("/assets///"), "/assets");

    EXPECT_TRUE(static_path::should_handle_path("/x", "/"));
    EXPECT_FALSE(static_path::should_handle_path("x", "/"));
    EXPECT_TRUE(static_path::should_handle_path("/assets", "/assets"));
    EXPECT_TRUE(
        static_path::should_handle_path("/assets/js/app.js", "/assets"));
    EXPECT_FALSE(
        static_path::should_handle_path("/assets2/app.js", "/assets"));

    EXPECT_EQ(
        static_path::map_to_relative_path("/assets/js/app.js", "/assets"),
        "/js/app.js");
    EXPECT_EQ(static_path::map_to_relative_path("/assets", "/assets"), "/");
    EXPECT_EQ(static_path::map_to_relative_path("/index.html", "/"),
              "/index.html");
}

TEST(HttpUtilsTest, StaticPathsSanitizeAndJoin) {
    std::string out;
    ASSERT_TRUE(static_path::sanitize_relative_path("/a//b/./c", out));
    EXPECT_EQ(out, "a/b/c");

    ASSERT_TRUE(static_path::sanitize_relative_path("a%2Fb", out));
    EXPECT_EQ(out, "a/b");

    EXPECT_FALSE(static_path::sanitize_relative_path("../secret", out));
    EXPECT_FALSE(static_path::sanitize_relative_path("%2e%2e/secret", out));
    EXPECT_FALSE(static_path::sanitize_relative_path("a/../b", out));

    EXPECT_EQ(static_path::join_path("/base", "x.txt"), "/base/x.txt");
    EXPECT_EQ(static_path::join_path("/base/", "x.txt"), "/base/x.txt");
    EXPECT_EQ(static_path::join_path("", "x.txt"), "x.txt");
    EXPECT_EQ(static_path::join_path("/base", ""), "/base");
}

TEST(HttpUtilsTest, FilesHandleReadWriteAndMetadata) {
    TempDir tmp;
    ASSERT_FALSE(tmp.path().empty());

    const std::string file_path = tmp.path() + "/data.bin";

    const std::string binary_payload("hello\0world", 11);
    EXPECT_TRUE(file::write_file_binary(file_path, binary_payload));

    std::string content;
    ASSERT_TRUE(file::read_file(file_path, content));
    EXPECT_EQ(content, binary_payload);

    EXPECT_TRUE(file::is_regular_file(file_path));
    EXPECT_FALSE(file::is_regular_file(tmp.path()));
    EXPECT_FALSE(file::is_regular_file("/tmp/not-exists-zhttp-file"));

    EXPECT_TRUE(file::is_directory(tmp.path()));
    EXPECT_FALSE(file::is_directory(file_path));
    EXPECT_FALSE(file::is_directory("/tmp/not-exists-zhttp-dir"));

    EXPECT_EQ(file::detect_content_type("index.html"), "text/html");
    EXPECT_EQ(file::detect_content_type("font.WOFF2"), "font/woff2");
    EXPECT_EQ(file::detect_content_type("README"),
              "application/octet-stream");
    EXPECT_EQ(file::detect_content_type("trailingdot."),
              "application/octet-stream");

    struct stat info = {};
    ASSERT_EQ(::stat(file_path.c_str(), &info), 0);
    const std::string last_modified = format_http_date_gmt(info.st_mtime);
    EXPECT_NE(last_modified.find("GMT"), std::string::npos);

    const std::string etag = file::get_etag(info);
    EXPECT_EQ(etag.find("W/\""), 0u);

}

} // namespace
} // namespace zhttp

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);

    zhttp::init_logger();
    return RUN_ALL_TESTS();
}
