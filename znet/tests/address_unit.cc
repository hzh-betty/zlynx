#include "znet/address.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <cstring>
#include <string>
#include <type_traits>

#include <gtest/gtest.h>

namespace znet {
namespace {

TEST(AddressUnitTest, LookupReturnsResultForLocalhost) {
    const auto v4 = Address::lookup("127.0.0.1", 8080, AF_INET);
    EXPECT_FALSE(v4.empty());

    const auto v6 = Address::lookup("::1", 9090, AF_INET6);
    EXPECT_FALSE(v6.empty());
}

TEST(AddressUnitTest, LookupReturnsEmptyOnInvalidHost) {
    const auto result = Address::lookup("host.invalid.znet.example", 10000);
    EXPECT_TRUE(result.empty());
}

TEST(AddressUnitTest, CreateHandlesNullAndUnknownFamily) {
    EXPECT_EQ(Address::create(nullptr, 0), nullptr);

    sockaddr_storage storage;
    std::memset(&storage, 0, sizeof(storage));
    storage.ss_family = AF_PACKET;
    EXPECT_EQ(Address::create(reinterpret_cast<const sockaddr *>(&storage),
                              sizeof(storage)),
              nullptr);
}

TEST(AddressUnitTest, CreateDispatchesToConcreteAddressTypes) {
    sockaddr_in v4;
    std::memset(&v4, 0, sizeof(v4));
    v4.sin_family = AF_INET;
    v4.sin_port = htons(1234);
    ASSERT_EQ(::inet_pton(AF_INET, "127.0.0.1", &v4.sin_addr), 1);
    auto addr4 = Address::create(reinterpret_cast<sockaddr *>(&v4), sizeof(v4));
    ASSERT_NE(addr4, nullptr);
    EXPECT_TRUE(std::dynamic_pointer_cast<IPv4Address>(addr4) != nullptr);

    sockaddr_in6 v6;
    std::memset(&v6, 0, sizeof(v6));
    v6.sin6_family = AF_INET6;
    v6.sin6_port = htons(4321);
    ASSERT_EQ(::inet_pton(AF_INET6, "::1", &v6.sin6_addr), 1);
    auto addr6 = Address::create(reinterpret_cast<sockaddr *>(&v6), sizeof(v6));
    ASSERT_NE(addr6, nullptr);
    EXPECT_TRUE(std::dynamic_pointer_cast<IPv6Address>(addr6) != nullptr);

    sockaddr_un un;
    std::memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    std::strncpy(un.sun_path, "/tmp/znet.sock", sizeof(un.sun_path) - 1);
    auto addru = Address::create(reinterpret_cast<sockaddr *>(&un), sizeof(un));
    ASSERT_NE(addru, nullptr);
    EXPECT_TRUE(std::dynamic_pointer_cast<UnixAddress>(addru) != nullptr);
}

TEST(AddressUnitTest, IPv4AndIPv6FallbackAndPortMutationWork) {
    IPv4Address ipv4("invalid-v4", 80);
    EXPECT_EQ(ipv4.to_string(), "0.0.0.0:80");
    EXPECT_EQ(ipv4.port(), 80);
    ipv4.set_port(8081);
    EXPECT_EQ(ipv4.port(), 8081);

    IPv6Address ipv6("invalid-v6", 81);
    EXPECT_EQ(ipv6.to_string(), "[::]:81");
    EXPECT_EQ(ipv6.port(), 81);
    ipv6.set_port(8082);
    EXPECT_EQ(ipv6.port(), 8082);
}

TEST(AddressUnitTest, UnixAddressTruncatesLongPathSafely) {
    UnixAddress addr;
    const std::string too_long(400, 'x');
    addr.set_path(too_long);

    const std::string actual = addr.path();
    EXPECT_FALSE(actual.empty());
    EXPECT_LT(actual.size(), too_long.size());
    EXPECT_EQ(addr.to_string(), actual);
}

} // namespace
} // namespace znet
