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

void removeDirRecursive(const std::string &path) {
    DIR *dir = opendir(path.c_str());
    if (!dir) {
        return;
    }
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        const std::string full = path + "/" + name;
        struct stat st {};
        if (stat(full.c_str(), &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            removeDirRecursive(full);
        } else {
            unlink(full.c_str());
        }
    }
    closedir(dir);
    rmdir(path.c_str());
}

std::string makeUniqueName(const char *prefix) {
    return std::string(prefix) + "_" + std::to_string(getpid()) + "_" +
           std::to_string(static_cast<long long>(std::time(NULL)));
}

} // namespace

class UtilTest : public ::testing::Test {};

TEST_F(UtilTest, PathReturnsDotWhenNoSeparator) {
    EXPECT_EQ(File::path("plain.log"), ".");
}

TEST_F(UtilTest, ExistsReturnsFalseForMissingPath) {
    const std::string missing = "/tmp/" + makeUniqueName("zlog_missing");
    EXPECT_FALSE(File::exists(missing));
}

TEST_F(UtilTest, ExistsReturnsTrueForExistingPath) {
    char templ[] = "/tmp/zlog_exist_XXXXXX";
    char *base = mkdtemp(templ);
    ASSERT_NE(base, static_cast<char *>(NULL));
    const std::string path(base);

    EXPECT_TRUE(File::exists(path));
    rmdir(path.c_str());
}

TEST_F(UtilTest, CreateDirectoryCreatesNestedPath) {
    char templ[] = "/tmp/zlog_util_XXXXXX";
    char *base = mkdtemp(templ);
    ASSERT_NE(base, static_cast<char *>(NULL));
    const std::string basePath(base);
    const std::string nested = basePath + "/p1/p2";

    File::create_directory(nested);

    EXPECT_TRUE(dirExists(basePath + "/p1"));
    EXPECT_TRUE(dirExists(nested));

    removeDirRecursive(basePath);
}

TEST_F(UtilTest, CreateDirectoryWithoutSeparatorCreatesLeafDir) {
    const std::string dirname = makeUniqueName("zlog_util_leaf");
    ASSERT_FALSE(dirExists(dirname));

    File::create_directory(dirname);

    EXPECT_TRUE(dirExists(dirname));
    rmdir(dirname.c_str());
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
