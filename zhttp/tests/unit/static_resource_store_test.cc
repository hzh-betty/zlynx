#include "static_files/static_resource_store.h"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <thread>
#include <unistd.h>
using namespace zhttp::detail;

namespace {
class ResourceDirectory {
  public:
    ResourceDirectory() {
        char path[] = "/tmp/zhttp-store-XXXXXX";
        auto *created = ::mkdtemp(path);
        if (!created)
            throw std::runtime_error("mkdtemp");
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
    StaticResourceStore store(directory.root, "index.html", true, policy,
                              [&] { return time; });
    auto one = store.lookup("one", false, {""},
                            StaticResourceStore::ReadMode::Content);
    ASSERT_TRUE(one.resource);
    auto two = store.lookup("two", false, {""},
                            StaticResourceStore::ReadMode::Content);
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

TEST(StaticResourceStoreTest,
     VariantSelectionAndFileOwnershipAreIndependentOfHttp) {
    ResourceDirectory directory;
    directory.write("app.js", "identity");
    directory.write("app.js.br", "brotli");
    StaticResourceStore store(directory.root, "index.html", true, {});
    auto resource = store.lookup("app.js", false, {"br", ""},
                                 StaticResourceStore::ReadMode::Metadata);
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

TEST(StaticResourceStoreTest, ExpiryBoundaryAndStaggeredEntries) {
    ResourceDirectory directory;
    directory.write("one", "old");
    directory.write("two", "two");
    auto time = StaticResourceStore::Clock::time_point{};
    StaticResourceStore store(directory.root, "index.html", false, {},
                              [&] { return time; });
    const auto mode = StaticResourceStore::ReadMode::Content;
    auto old = store.lookup("one", false, {""}, mode).resource;
    ASSERT_TRUE(old);
    time += std::chrono::seconds{2};
    ASSERT_TRUE(store.lookup("two", false, {""}, mode).resource);
    directory.write("one", "new");
    time += std::chrono::seconds{3};
    store.clear_expired();
    EXPECT_EQ(store.cached_entries(), 2U);
    EXPECT_EQ(store.lookup("one", false, {""}, mode).resource, old);
    time += std::chrono::nanoseconds{1};
    store.clear_expired();
    EXPECT_EQ(store.cached_entries(), 1U);
    auto refreshed = store.lookup("one", false, {""}, mode).resource;
    ASSERT_TRUE(refreshed);
    EXPECT_EQ(*refreshed->memory, "new");
    EXPECT_EQ(*old->memory, "old");
    time = StaticResourceStore::Clock::time_point{} + std::chrono::seconds{7};
    store.clear_expired();
    EXPECT_EQ(store.cached_entries(), 2U);
    time += std::chrono::nanoseconds{1};
    store.clear_expired();
    EXPECT_EQ(store.cached_entries(), 1U);
}

TEST(StaticResourceStoreTest, EvictionLeavesConservativeExpiryBound) {
    ResourceDirectory directory;
    directory.write("one", "one");
    directory.write("two", "two");
    auto time = StaticResourceStore::Clock::time_point{};
    StaticResourceStore::CachePolicy policy;
    policy.max_entries = 1;
    StaticResourceStore store(directory.root, "index.html", false, policy,
                              [&] { return time; });
    ASSERT_TRUE(
        store.lookup("one", false, {""}, StaticResourceStore::ReadMode::Content)
            .resource);
    time += std::chrono::seconds{2};
    ASSERT_TRUE(
        store.lookup("two", false, {""}, StaticResourceStore::ReadMode::Content)
            .resource);
    time += std::chrono::seconds{4};
    store.clear_expired();
    EXPECT_EQ(store.cached_entries(), 1U);
    time += std::chrono::seconds{2};
    store.clear_expired();
    EXPECT_EQ(store.cached_entries(), 0U);

    // 清空后的新写入必须恢复到正确的过期下界。
    ASSERT_TRUE(
        store.lookup("one", false, {""}, StaticResourceStore::ReadMode::Content)
            .resource);
    time += std::chrono::seconds{6};
    store.clear_expired();
    EXPECT_EQ(store.cached_entries(), 0U);
}

TEST(StaticResourceStoreTest, ConcurrentLookupsAndExpiryKeepSnapshotsAlive) {
    ResourceDirectory directory;
    directory.write("one", "one");
    std::atomic<int> seconds{0};
    StaticResourceStore store(directory.root, "index.html", false, {}, [&] {
        return StaticResourceStore::Clock::time_point{} +
               std::chrono::seconds{seconds.load()};
    });
    auto snapshot =
        store.lookup("one", false, {""}, StaticResourceStore::ReadMode::Content)
            .resource;
    ASSERT_TRUE(snapshot);
    std::vector<std::thread> readers;
    std::atomic<bool> valid{true};
    for (int i = 0; i < 4; ++i)
        readers.emplace_back([&] {
            for (int j = 0; j < 1000; ++j) {
                auto result = store.lookup(
                    "one", false, {""}, StaticResourceStore::ReadMode::Content);
                if (!result.resource || !result.resource->memory ||
                    *result.resource->memory != "one")
                    valid.store(false);
            }
        });
    for (int i = 1; i <= 100; ++i) {
        seconds.store(i);
        store.clear_expired();
    }
    for (auto &reader : readers)
        reader.join();
    EXPECT_TRUE(valid.load());
    EXPECT_EQ(*snapshot->memory, "one");
    seconds.store(200);
    store.clear_expired();
    EXPECT_EQ(store.cached_bytes(), 0U);
}
} // namespace
