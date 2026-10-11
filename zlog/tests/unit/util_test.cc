#include "../support/files.h"
#include "zlog/internal/util.h"

#include <dirent.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ctime>
#include <string>

using namespace zlog;

namespace {

bool dirExists(const std::string &path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string makeUniqueName(const char *prefix) {
    return std::string(prefix) + "_" + std::to_string(getpid()) + "_" +
           std::to_string(static_cast<long long>(std::time(NULL)));
}

} // namespace

TEST(UtilTest, PathReturnsDotWhenNoSeparator) {
    EXPECT_EQ(File::path("plain.log"), ".");
}

TEST(UtilTest, PathReturnsParentWithUnixAndWindowsSeparators) {
    EXPECT_EQ(File::path("/tmp/zlog/plain.log"), "/tmp/zlog/");
    EXPECT_EQ(File::path("C:\\tmp\\zlog\\plain.log"), "C:\\tmp\\zlog\\");
}

TEST(UtilTest, ExistsReturnsFalseForMissingPath) {
    const std::string missing = "/tmp/" + makeUniqueName("zlog_missing");
    EXPECT_FALSE(File::exists(missing));
}

TEST(UtilTest, ExistsReturnsTrueForExistingPath) {
    test::TempDirectory directory;
    const std::string path = directory.path();

    EXPECT_TRUE(File::exists(path));
}

TEST(UtilTest, CreateDirectoryCreatesNestedPath) {
    test::TempDirectory directory;
    const std::string basePath = directory.path();
    const std::string nested = basePath + "/p1/p2";

    File::create_directory(nested);

    EXPECT_TRUE(dirExists(basePath + "/p1"));
    EXPECT_TRUE(dirExists(nested));
}

TEST(UtilTest, CreateDirectoryWithoutSeparatorCreatesLeafDir) {
    const std::string dirname = makeUniqueName("zlog_util_leaf");
    ASSERT_FALSE(dirExists(dirname));

    struct Cleanup {
        std::string path;

        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{dirname};
    File::create_directory(dirname);

    EXPECT_TRUE(dirExists(dirname));
    rmdir(dirname.c_str());
}

TEST(UtilTest, CreateDirectoryIsIdempotentForExistingPath) {
    test::TempDirectory directory;
    const std::string basePath = directory.path();
    const std::string nested = basePath + "/p1/p2/";

    File::create_directory(nested);
    File::create_directory(nested);

    EXPECT_TRUE(dirExists(basePath + "/p1"));
    EXPECT_TRUE(dirExists(basePath + "/p1/p2"));
}
