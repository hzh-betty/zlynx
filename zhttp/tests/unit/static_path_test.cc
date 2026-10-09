#include "zhttp/http_common.h"
#include "static_files/path.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>

namespace zhttp {
namespace {

TEST(StaticPathTest, StaticPathsNormalizeMatchAndMap) {
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

TEST(StaticPathTest, StaticPathsSanitizeAndJoin) {
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

} // namespace
} // namespace zhttp

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);

    return RUN_ALL_TESTS();
}
