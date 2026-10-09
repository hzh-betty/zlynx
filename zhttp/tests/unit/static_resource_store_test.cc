#include "static_files/static_resource_store.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <unistd.h>
using namespace zhttp::detail;
namespace {
class ResourceDirectory {
  public:
    ResourceDirectory() {
        char path[] = "/tmp/zhttp-store-XXXXXX";
        auto *created = ::mkdtemp(path);
        if (!created) throw std::runtime_error("mkdtemp");
        root = created;
    }
    ~ResourceDirectory() { std::filesystem::remove_all(root); }
    void write(const std::string &name, const std::string &bytes) {
        std::ofstream(root + "/" + name, std::ios::binary) << bytes;
    }
    std::string root;
};
TEST(StaticResourceStoreTest, BoundsCacheAndSnapshotsSurviveEvictionAndExpiry) {
    ResourceDirectory directory;
    directory.write("one", "1111");
    directory.write("two", "2222");
    auto time = StaticResourceStore::Clock::now();
    StaticResourceStore::CachePolicy policy;
    policy.max_bytes = 4;
    policy.max_entries = 1;
    StaticResourceStore store(directory.root, "index.html", true, policy, [&] { return time; });
    auto one = store.lookup("one", false, {""}, StaticResourceStore::ReadMode::Content);
    ASSERT_TRUE(one.resource);
    auto two = store.lookup("two", false, {""}, StaticResourceStore::ReadMode::Content);
    ASSERT_TRUE(two.resource);
    EXPECT_EQ(store.cached_bytes(), 4U);
    EXPECT_EQ(store.cached_entries(), 1U);
    EXPECT_EQ(*one.resource->memory, "1111");
    time += std::chrono::seconds{6};
    store.clear_expired();
    EXPECT_EQ(store.cached_bytes(), 0U);
    EXPECT_EQ(store.cached_entries(), 0U);
    EXPECT_EQ(*two.resource->memory, "2222");
}
TEST(StaticResourceStoreTest, VariantSelectionAndFileOwnershipAreIndependentOfHttp) {
    ResourceDirectory directory;
    directory.write("app.js", "identity");
    directory.write("app.js.br", "brotli");
    StaticResourceStore store(directory.root, "index.html", true, {});
    auto resource = store.lookup("app.js", false, {"br", ""}, StaticResourceStore::ReadMode::Metadata);
    ASSERT_EQ(resource.state, ResourceLookup::State::Found);
    EXPECT_EQ(resource.resource->encoding, "br");
    EXPECT_EQ(resource.resource->etag.find("W/\"6-"), 0U);
    EXPECT_EQ(resource.resource->content_type, "application/javascript");
    auto body = StaticResourceStore::body(*resource.resource, 1, 3);
    std::filesystem::remove(directory.root + "/app.js.br");
    char bytes[3];
    ASSERT_EQ(::pread(body.file_resource()->fd, bytes, 3, 1), 3);
    EXPECT_EQ(std::string(bytes, 3), "rot");
    EXPECT_FALSE(resource.resource->memory);
}
}
int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
