#include "zhttp/websocket_frame.h"
#include "zhttp/zhttp_logger.h"

#include <gtest/gtest.h>

namespace zhttp {
namespace {

std::string build_masked_client_frame(WebSocketOpcode opcode,
                                      const std::string &payload,
                                      bool fin = true) {
    std::string frame;
    const uint8_t first = static_cast<uint8_t>((fin ? 0x80 : 0x00) |
                                               static_cast<uint8_t>(opcode));
    frame.push_back(static_cast<char>(first));

    const uint8_t mask_key[4] = {0x37, 0xFA, 0x21, 0x3D};

    if (payload.size() <= 125) {
        frame.push_back(static_cast<char>(0x80 | payload.size()));
    } else if (payload.size() <= 65535) {
        frame.push_back(static_cast<char>(0x80 | 126));
        frame.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
        frame.push_back(static_cast<char>(payload.size() & 0xFF));
    }

    frame.append(reinterpret_cast<const char *>(mask_key), sizeof(mask_key));
    for (size_t i = 0; i < payload.size(); ++i) {
        frame.push_back(static_cast<char>(payload[i] ^ mask_key[i % 4]));
    }

    return frame;
}

TEST(WebSocketFrameTest, ParsesMaskedTextFrame) {
    WebSocketFrameParser parser;
    znet::Buffer buffer;
    buffer.append(build_masked_client_frame(WebSocketOpcode::kText, "hello"));

    std::vector<WebSocketFrameEvent> events;
    uint16_t close_code = 0;
    std::string error;

    const bool ok = parser.parse(&buffer, &events, &close_code, &error);

    EXPECT_TRUE(ok);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].opcode, WebSocketOpcode::kText);
    EXPECT_EQ(events[0].payload, "hello");
}

TEST(WebSocketFrameTest, ReassemblesFragmentedTextFrames) {
    WebSocketFrameParser parser;
    znet::Buffer buffer;
    buffer.append(
        build_masked_client_frame(WebSocketOpcode::kText, "Hel", false));
    buffer.append(
        build_masked_client_frame(WebSocketOpcode::kContinuation, "lo", true));

    std::vector<WebSocketFrameEvent> events;
    uint16_t close_code = 0;
    std::string error;

    const bool ok = parser.parse(&buffer, &events, &close_code, &error);

    EXPECT_TRUE(ok);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].opcode, WebSocketOpcode::kText);
    EXPECT_EQ(events[0].payload, "Hello");
}

TEST(WebSocketFrameTest, RejectsClientUnmaskedFrame) {
    WebSocketFrameParser parser;
    znet::Buffer buffer;

    std::string frame;
    frame.push_back(static_cast<char>(0x81));
    frame.push_back(static_cast<char>(0x02));
    frame.push_back('o');
    frame.push_back('k');
    buffer.append(frame);

    std::vector<WebSocketFrameEvent> events;
    uint16_t close_code = 0;
    std::string error;

    const bool ok = parser.parse(&buffer, &events, &close_code, &error);

    EXPECT_FALSE(ok);
    EXPECT_EQ(close_code,
              static_cast<uint16_t>(WebSocketCloseCode::kProtocolError));
    EXPECT_FALSE(error.empty());
}

TEST(WebSocketFrameTest, RejectsInvalidUtf8TextFrame) {
    WebSocketFrameParser parser;
    znet::Buffer buffer;

    const std::string invalid_utf8("\xC3\x28", 2);
    buffer.append(
        build_masked_client_frame(WebSocketOpcode::kText, invalid_utf8));

    std::vector<WebSocketFrameEvent> events;
    uint16_t close_code = 0;
    std::string error;

    const bool ok = parser.parse(&buffer, &events, &close_code, &error);

    EXPECT_FALSE(ok);
    EXPECT_EQ(close_code, static_cast<uint16_t>(
                              WebSocketCloseCode::kInvalidFramePayloadData));
    EXPECT_FALSE(error.empty());
}

TEST(WebSocketFrameTest, RejectsInvalidCloseStatusCode) {
    WebSocketFrameParser parser;
    znet::Buffer buffer;

    std::string close_payload;
    close_payload.push_back(static_cast<char>(0x03));
    close_payload.push_back(
        static_cast<char>(0xED)); // 1005 is reserved/invalid on wire.
    buffer.append(
        build_masked_client_frame(WebSocketOpcode::kClose, close_payload));

    std::vector<WebSocketFrameEvent> events;
    uint16_t close_code = 0;
    std::string error;

    const bool ok = parser.parse(&buffer, &events, &close_code, &error);

    EXPECT_FALSE(ok);
    EXPECT_EQ(close_code,
              static_cast<uint16_t>(WebSocketCloseCode::kProtocolError));
    EXPECT_FALSE(error.empty());
}

TEST(WebSocketFrameTest, RejectsInvalidUtf8CloseReason) {
    WebSocketFrameParser parser;
    znet::Buffer buffer;

    std::string close_payload;
    close_payload.push_back(static_cast<char>(0x03));
    close_payload.push_back(static_cast<char>(0xE8)); // 1000 normal closure.
    close_payload.push_back(static_cast<char>(0xC3));
    close_payload.push_back(static_cast<char>(0x28));
    buffer.append(
        build_masked_client_frame(WebSocketOpcode::kClose, close_payload));

    std::vector<WebSocketFrameEvent> events;
    uint16_t close_code = 0;
    std::string error;

    const bool ok = parser.parse(&buffer, &events, &close_code, &error);

    EXPECT_FALSE(ok);
    EXPECT_EQ(close_code, static_cast<uint16_t>(
                              WebSocketCloseCode::kInvalidFramePayloadData));
    EXPECT_FALSE(error.empty());
}

TEST(WebSocketFrameTest, EncodesServerFrameWithoutMask) {
    std::string frame;
    const bool ok =
        build_websocket_frame(WebSocketOpcode::kText, "hey", &frame, true);

    ASSERT_TRUE(ok);
    ASSERT_GE(frame.size(), 5u);
    EXPECT_EQ(static_cast<uint8_t>(frame[0]), 0x81);
    EXPECT_EQ(static_cast<uint8_t>(frame[1]), 0x03);
    EXPECT_EQ(frame.substr(2), "hey");
}

} // namespace
} // namespace zhttp

int main(int argc, char **argv) {
    zhttp::init_logger();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
