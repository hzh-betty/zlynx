#include "znet/tls_context.h"

#include <gtest/gtest.h>

#include <string>

namespace znet {
namespace {

TEST(TlsContextUnitTest, CreateServerContextRejectsInvalidCertificatePaths) {
    std::string error;
    auto ctx = create_server_tls_context_openssl(
        "/tmp/not-exist-cert.pem", "/tmp/not-exist-key.pem", &error);

    EXPECT_EQ(ctx, nullptr);
    EXPECT_FALSE(error.empty());
}

} // namespace
} // namespace znet
