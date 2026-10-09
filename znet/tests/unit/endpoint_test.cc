#include "znet/endpoint.h"
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/un.h>

using namespace znet;

TEST(EndpointTest, ValidatesAddressesInsteadOfSilentlyBindingAny) {
    auto ipv4 = Endpoint::ipv4("127.0.0.1", 8080);
    auto ipv6 = Endpoint::ipv6("::1", 8081);
    ASSERT_TRUE(ipv4);
    ASSERT_TRUE(ipv6);
    EXPECT_EQ(ipv4.value().to_string(), "127.0.0.1:8080");
    EXPECT_EQ(ipv6.value().to_string(), "[::1]:8081");
    EXPECT_EQ(ipv4.value().port(), 8080);
    EXPECT_EQ(ipv6.value().family(), AF_INET6);
    EXPECT_FALSE(Endpoint::ipv4("invalid", 80));
    EXPECT_FALSE(Endpoint::ipv6("invalid", 80));
    EXPECT_FALSE(Endpoint::ipv4(std::string("127.0.0.1\0junk", 14), 80));
}

TEST(EndpointTest, RejectsTruncatedNativeAddressesAndKeepsValueOwnership) {
    sockaddr_in raw{};
    raw.sin_family = AF_INET;
    raw.sin_port = htons(99);
    EXPECT_FALSE(Endpoint::from_native(nullptr, 0));
    EXPECT_FALSE(Endpoint::from_native(reinterpret_cast<sockaddr *>(&raw),
                                       sizeof(sa_family_t)));
    auto result =
        Endpoint::from_native(reinterpret_cast<sockaddr *>(&raw), sizeof(raw));
    ASSERT_TRUE(result);
    raw.sin_port = htons(1);
    EXPECT_EQ(result.value().port(), 99);
    raw.sin_family = AF_PACKET;
    EXPECT_FALSE(
        Endpoint::from_native(reinterpret_cast<sockaddr *>(&raw), sizeof(raw)));
}

TEST(EndpointTest, PreservesExactUnixLengthsAndAbstractNames) {
    auto path = Endpoint::unix_domain("/tmp/znet.sock");
    ASSERT_TRUE(path);
    EXPECT_EQ(path.value().to_string(), "/tmp/znet.sock");
    EXPECT_EQ(path.value().native_size(), offsetof(sockaddr_un, sun_path) + 15);
    const std::string abstract("\0znet\0test", 10);
    auto endpoint = Endpoint::unix_domain(abstract);
    ASSERT_TRUE(endpoint);
    EXPECT_EQ(endpoint.value().native_size(),
              offsetof(sockaddr_un, sun_path) + abstract.size());
    EXPECT_EQ(endpoint.value().to_string(), "@" + abstract.substr(1));
    EXPECT_FALSE(Endpoint::unix_domain(std::string(108, 'x')));
    EXPECT_TRUE(
        Endpoint::unix_domain(std::string("\0", 1) + std::string(107, 'x')));
    EXPECT_FALSE(Endpoint::unix_domain(std::string("a\0b", 3)));
    auto unnamed = Endpoint::unix_domain("");
    ASSERT_TRUE(unnamed);
    EXPECT_TRUE(unnamed.value().to_string().empty());
}

TEST(EndpointTest, ResolutionReportsErrorsAndReturnsValues) {
    auto result = resolve_endpoints("127.0.0.1", 1234, AF_INET);
    ASSERT_TRUE(result);
    ASSERT_FALSE(result.value().empty());
    EXPECT_EQ(result.value().front().port(), 1234);
    EXPECT_FALSE(resolve_endpoints("invalid host name", 1234));
}
