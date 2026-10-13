#include "znet/transport/tls_credentials.h"
#include <gtest/gtest.h>

using namespace znet;

TEST(TlsCredentialsTest, ReportsUnavailableWhenTlsDisabled) {
#if ZNET_ENABLE_TLS
    GTEST_SKIP() << "TLS enabled build";
#else
    auto loaded = TlsCredentials::load("cert.pem", "key.pem");
    ASSERT_FALSE(loaded);
    EXPECT_EQ(loaded.error().kind, ErrorKind::runtime);
    EXPECT_EQ(loaded.error().code, std::errc::operation_not_supported);
    EXPECT_NE(loaded.error().message().find("without TLS support"),
              std::string::npos);
#endif
}
