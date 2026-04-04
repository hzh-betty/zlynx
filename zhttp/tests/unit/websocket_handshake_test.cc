#include "http_request.h"
#include "websocket.h"
#include "zhttp_logger.h"

#include <gtest/gtest.h>

using namespace zhttp;

TEST(WebSocketHandshakeTest, ComputesAcceptKeyFromRfcExample) {
  const std::string accept_key =
      compute_websocket_accept_key("dGhlIHNhbXBsZSBub25jZQ==");

  EXPECT_EQ(accept_key, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST(WebSocketHandshakeTest, RejectsUnsupportedWebSocketVersion) {
  auto request = std::make_shared<HttpRequest>();
  request->set_method(HttpMethod::GET);
  request->set_version(HttpVersion::HTTP_1_1);
  request->set_header("Connection", "Upgrade");
  request->set_header("Upgrade", "websocket");
  request->set_header("Sec-WebSocket-Version", "12");
  request->set_header("Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");

  std::string error;
  const WebSocketHandshakeResult result =
      check_websocket_handshake_request(request, &error);

  EXPECT_EQ(result, WebSocketHandshakeResult::kUnsupportedVersion);
  EXPECT_FALSE(error.empty());
}

TEST(WebSocketHandshakeTest, BuildsSwitchingProtocolsResponse) {
  auto request = std::make_shared<HttpRequest>();
  request->set_method(HttpMethod::GET);
  request->set_version(HttpVersion::HTTP_1_1);
  request->set_header("Connection", "keep-alive, Upgrade");
  request->set_header("Upgrade", "websocket");
  request->set_header("Sec-WebSocket-Version", "13");
  request->set_header("Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");

  std::string response;
  std::string error;
  const bool ok = build_websocket_handshake_response(request, &response, &error);

  EXPECT_TRUE(ok);
  EXPECT_TRUE(error.empty());
  EXPECT_NE(response.find("HTTP/1.1 101 Switching Protocols"), std::string::npos)
      << response;
  EXPECT_NE(response.find("Upgrade: websocket"), std::string::npos) << response;
  EXPECT_NE(response.find("Connection: Upgrade"), std::string::npos) << response;
  EXPECT_NE(response.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo="),
            std::string::npos)
      << response;
}

  TEST(WebSocketHandshakeTest, BuildsResponseWithNegotiatedSubprotocol) {
    auto request = std::make_shared<HttpRequest>();
    request->set_method(HttpMethod::GET);
    request->set_version(HttpVersion::HTTP_1_1);
    request->set_header("Connection", "Upgrade");
    request->set_header("Upgrade", "websocket");
    request->set_header("Sec-WebSocket-Version", "13");
    request->set_header("Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");
    request->set_header("Sec-WebSocket-Protocol", "chat, superchat");

    std::string response;
    std::string error;
    const bool ok = build_websocket_handshake_response(
    request, &response, &error, "superchat");

    EXPECT_TRUE(ok);
    EXPECT_TRUE(error.empty());
    EXPECT_NE(response.find("Sec-WebSocket-Protocol: superchat"),
      std::string::npos)
    << response;
  }

int main(int argc, char **argv) {
  zhttp::init_logger();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
