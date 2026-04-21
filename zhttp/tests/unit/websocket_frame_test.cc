#include "zhttp/websocket_frame.h"
#include "zhttp/zhttp_logger.h"

#include <gtest/gtest.h>

namespace zhttp {
namespace {

void append_u16_be(std::string *out, uint16_t value) {
    out->push_back(static_cast<char>((value >> 8) & 0xFF));
    out->push_back(static_cast<char>(value & 0xFF));
}

void append_u64_be(std::string *out, uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out->push_back(static_cast<char>((value >> shift) & 0xFF));
    }
}

std::string build_masked_client_frame(WebSocketOpcode opcode,
                                      const std::string &payload,
                                      bool fin = true,
                                      int force_payload_len_code = 0) {
    std::string frame;
    const uint8_t first = static_cast<uint8_t>((fin ? 0x80 : 0x00) |
                                               static_cast<uint8_t>(opcode));
    frame.push_back(static_cast<char>(first));

    const uint8_t mask_key[4] = {0x37, 0xFA, 0x21, 0x3D};

    if (force_payload_len_code == 127 || payload.size() > 0xFFFF) {
        frame.push_back(static_cast<char>(0x80 | 127));
        append_u64_be(&frame, static_cast<uint64_t>(payload.size()));
    } else if (force_payload_len_code == 126 || payload.size() > 125) {
        frame.push_back(static_cast<char>(0x80 | 126));
        append_u16_be(&frame, static_cast<uint16_t>(payload.size()));
    } else {
        frame.push_back(static_cast<char>(0x80 | payload.size()));
    }

    frame.append(reinterpret_cast<const char *>(mask_key), sizeof(mask_key));
    for (size_t i = 0; i < payload.size(); ++i) {
        frame.push_back(static_cast<char>(payload[i] ^ mask_key[i % 4]));
    }

    return frame;
}

TEST(WebSocketFrameTest, ReturnsFalseWhenRequiredOutputPointersAreNull) {
    WebSocketFrameParser parser;
    znet::Buffer buffer;
    std::vector<WebSocketFrameEvent> events;
    uint16_t close_code = 0;
    std::string error;

    EXPECT_FALSE(parser.parse(nullptr, &events, &close_code, &error));
    EXPECT_FALSE(parser.parse(&buffer, nullptr, &close_code, &error));
    EXPECT_FALSE(parser.parse(&buffer, &events, nullptr, &error));
    EXPECT_FALSE(parser.parse(&buffer, &events, &close_code, nullptr));
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

TEST(WebSocketFrameTest, ParsesExtendedLengthFrames) {
    WebSocketFrameParser parser;
    znet::Buffer buffer;

    const std::string payload_126(126, 'a');
    const std::string payload_127(66000, 'b');
    buffer.append(
        build_masked_client_frame(WebSocketOpcode::kBinary, payload_126));
    buffer.append(
        build_masked_client_frame(WebSocketOpcode::kBinary, payload_127));

    std::vector<WebSocketFrameEvent> events;
    uint16_t close_code = 0;
    std::string error;

    const bool ok = parser.parse(&buffer, &events, &close_code, &error);

    EXPECT_TRUE(ok);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].opcode, WebSocketOpcode::kBinary);
    EXPECT_EQ(events[0].payload, payload_126);
    EXPECT_EQ(events[1].opcode, WebSocketOpcode::kBinary);
    EXPECT_EQ(events[1].payload, payload_127);
}

TEST(WebSocketFrameTest, LeavesDataUntouchedWhenExtendedHeaderIncomplete) {
    WebSocketFrameParser parser;
    znet::Buffer buffer;

    std::string partial;
    partial.push_back(static_cast<char>(0x82)); // FIN + binary
    partial.push_back(static_cast<char>(0x80 | 126));
    partial.push_back(static_cast<char>(0x00)); // missing one byte of length
    buffer.append(partial);

    std::vector<WebSocketFrameEvent> events;
    uint16_t close_code = 0;
    std::string error;

    const size_t before = buffer.readable_bytes();
    const bool ok = parser.parse(&buffer, &events, &close_code, &error);

    EXPECT_TRUE(ok);
    EXPECT_TRUE(events.empty());
    EXPECT_EQ(buffer.readable_bytes(), before);
    EXPECT_EQ(close_code,
              static_cast<uint16_t>(WebSocketCloseCode::kNormalClosure));
    EXPECT_TRUE(error.empty());
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

TEST(WebSocketFrameTest, RejectsRsvBitsAndUnknownOpcode) {
    WebSocketFrameParser parser;
    std::vector<WebSocketFrameEvent> events;
    uint16_t close_code = 0;
    std::string error;

    znet::Buffer rsv_buffer;
    std::string rsv_frame;
    rsv_frame.push_back(static_cast<char>(0xC1)); // RSV + text
    rsv_frame.push_back(static_cast<char>(0x80));
    rsv_frame.append("\x11\x22\x33\x44", 4);
    rsv_buffer.append(rsv_frame);

    EXPECT_FALSE(parser.parse(&rsv_buffer, &events, &close_code, &error));
    EXPECT_EQ(close_code,
              static_cast<uint16_t>(WebSocketCloseCode::kProtocolError));
    EXPECT_NE(error.find("RSV"), std::string::npos);

    znet::Buffer opcode_buffer;
    std::string bad_opcode_frame;
    bad_opcode_frame.push_back(static_cast<char>(0x83)); // unknown opcode=3
    bad_opcode_frame.push_back(static_cast<char>(0x80));
    bad_opcode_frame.append("\x11\x22\x33\x44", 4);
    opcode_buffer.append(bad_opcode_frame);

    EXPECT_FALSE(parser.parse(&opcode_buffer, &events, &close_code, &error));
    EXPECT_EQ(close_code,
              static_cast<uint16_t>(WebSocketCloseCode::kProtocolError));
    EXPECT_NE(error.find("Unknown"), std::string::npos);
}

TEST(WebSocketFrameTest, RejectsInvalid64BitPayloadLengthPrefix) {
    WebSocketFrameParser parser;
    znet::Buffer buffer;

    std::string frame;
    frame.push_back(static_cast<char>(0x82)); // FIN + binary
    frame.push_back(static_cast<char>(0x80 | 127));
    frame.push_back(static_cast<char>(0x80)); // highest bit must be zero
    frame.append(7, '\0');
    buffer.append(frame);

    std::vector<WebSocketFrameEvent> events;
    uint16_t close_code = 0;
    std::string error;

    const bool ok = parser.parse(&buffer, &events, &close_code, &error);
    EXPECT_FALSE(ok);
    EXPECT_EQ(close_code,
              static_cast<uint16_t>(WebSocketCloseCode::kProtocolError));
    EXPECT_NE(error.find("payload length"), std::string::npos);
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

TEST(WebSocketFrameTest, AcceptsUtf8BoundarySequences) {
    WebSocketFrameParser parser;
    znet::Buffer buffer;

    std::string utf8_payload = "x";
    utf8_payload.append(std::string("\xC2\xA2", 2));         // 2-byte
    utf8_payload.append(std::string("\xE0\xA0\x80", 3));     // E0 branch
    utf8_payload.append(std::string("\xE1\x80\x80", 3));     // E1-EC branch
    utf8_payload.append(std::string("\xED\x9F\xBF", 3));     // ED branch
    utf8_payload.append(std::string("\xEE\x80\x80", 3));     // EE-EF branch
    utf8_payload.append(std::string("\xF0\x90\x80\x80", 4)); // F0 branch
    utf8_payload.append(std::string("\xF1\x80\x80\x80", 4)); // F1-F3 branch
    utf8_payload.append(std::string("\xF4\x8F\xBF\xBF", 4)); // F4 branch

    buffer.append(
        build_masked_client_frame(WebSocketOpcode::kText, utf8_payload));

    std::vector<WebSocketFrameEvent> events;
    uint16_t close_code = 0;
    std::string error;
    const bool ok = parser.parse(&buffer, &events, &close_code, &error);

    ASSERT_TRUE(ok);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].opcode, WebSocketOpcode::kText);
    EXPECT_EQ(events[0].payload, utf8_payload);
    EXPECT_TRUE(error.empty());
}

TEST(WebSocketFrameTest, RejectsUtf8BoundaryViolations) {
    struct Case {
        const char *name;
        std::string payload;
    };

    const std::vector<Case> cases = {
        {"invalid_e0_second", std::string("\xE0\x9F\x80", 3)},
        {"invalid_ed_second", std::string("\xED\xA0\x80", 3)},
        {"invalid_f0_second", std::string("\xF0\x8F\x80\x80", 4)},
        {"invalid_f4_second", std::string("\xF4\x90\x80\x80", 4)},
    };

    for (const auto &test_case : cases) {
        SCOPED_TRACE(test_case.name);
        WebSocketFrameParser parser;
        znet::Buffer buffer;
        buffer.append(build_masked_client_frame(WebSocketOpcode::kText,
                                                test_case.payload));

        std::vector<WebSocketFrameEvent> events;
        uint16_t close_code = 0;
        std::string error;
        const bool ok = parser.parse(&buffer, &events, &close_code, &error);

        EXPECT_FALSE(ok);
        EXPECT_EQ(close_code,
                  static_cast<uint16_t>(
                      WebSocketCloseCode::kInvalidFramePayloadData));
        EXPECT_NE(error.find("UTF-8"), std::string::npos);
    }
}

TEST(WebSocketFrameTest, RejectsInvalidFragmentationSemantics) {
    WebSocketFrameParser parser;
    std::vector<WebSocketFrameEvent> events;
    uint16_t close_code = 0;
    std::string error;

    znet::Buffer continuation_first;
    continuation_first.append(
        build_masked_client_frame(WebSocketOpcode::kContinuation, "x"));
    EXPECT_FALSE(
        parser.parse(&continuation_first, &events, &close_code, &error));
    EXPECT_EQ(close_code,
              static_cast<uint16_t>(WebSocketCloseCode::kProtocolError));

    znet::Buffer mid_data_restart;
    mid_data_restart.append(
        build_masked_client_frame(WebSocketOpcode::kText, "ab", false));
    mid_data_restart.append(
        build_masked_client_frame(WebSocketOpcode::kBinary, "c", true));
    EXPECT_FALSE(parser.parse(&mid_data_restart, &events, &close_code, &error));
    EXPECT_EQ(close_code,
              static_cast<uint16_t>(WebSocketCloseCode::kProtocolError));
}

TEST(WebSocketFrameTest, ValidatesUtf8OnlyWhenFinalFragmentArrives) {
    {
        WebSocketFrameParser parser;
        znet::Buffer buffer;
        buffer.append(build_masked_client_frame(WebSocketOpcode::kText,
                                                "\xE4\xB8", false));
        buffer.append(build_masked_client_frame(WebSocketOpcode::kContinuation,
                                                "\xAD", true));

        std::vector<WebSocketFrameEvent> events;
        uint16_t close_code = 0;
        std::string error;
        const bool ok = parser.parse(&buffer, &events, &close_code, &error);

        ASSERT_TRUE(ok);
        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events[0].opcode, WebSocketOpcode::kText);
        EXPECT_EQ(events[0].payload, std::string("\xE4\xB8\xAD", 3));
    }

    {
        WebSocketFrameParser parser;
        znet::Buffer buffer;
        buffer.append(build_masked_client_frame(WebSocketOpcode::kText,
                                                "\xE4\xB8", false));
        buffer.append(build_masked_client_frame(WebSocketOpcode::kContinuation,
                                                "(", true));

        std::vector<WebSocketFrameEvent> events;
        uint16_t close_code = 0;
        std::string error;
        const bool ok = parser.parse(&buffer, &events, &close_code, &error);

        EXPECT_FALSE(ok);
        EXPECT_EQ(close_code,
                  static_cast<uint16_t>(
                      WebSocketCloseCode::kInvalidFramePayloadData));
        EXPECT_NE(error.find("UTF-8"), std::string::npos);
    }
}

TEST(WebSocketFrameTest, RejectsControlFrameViolations) {
    WebSocketFrameParser parser;
    std::vector<WebSocketFrameEvent> events;
    uint16_t close_code = 0;
    std::string error;

    znet::Buffer fragmented_ping;
    fragmented_ping.append(
        build_masked_client_frame(WebSocketOpcode::kPing, "x", false));
    EXPECT_FALSE(parser.parse(&fragmented_ping, &events, &close_code, &error));
    EXPECT_EQ(close_code,
              static_cast<uint16_t>(WebSocketCloseCode::kProtocolError));

    znet::Buffer long_ping;
    long_ping.append(build_masked_client_frame(WebSocketOpcode::kPing,
                                               std::string(126, 'x')));
    EXPECT_FALSE(parser.parse(&long_ping, &events, &close_code, &error));
    EXPECT_EQ(close_code,
              static_cast<uint16_t>(WebSocketCloseCode::kProtocolError));
}

TEST(WebSocketFrameTest, RejectsMessageSizeOverflowPaths) {
    WebSocketFrameParser parser(4);
    std::vector<WebSocketFrameEvent> events;
    uint16_t close_code = 0;
    std::string error;

    znet::Buffer single_frame_too_large;
    single_frame_too_large.append(
        build_masked_client_frame(WebSocketOpcode::kText, "12345"));
    EXPECT_FALSE(
        parser.parse(&single_frame_too_large, &events, &close_code, &error));
    EXPECT_EQ(close_code,
              static_cast<uint16_t>(WebSocketCloseCode::kMessageTooLarge));

    znet::Buffer fragmented_too_large;
    fragmented_too_large.append(
        build_masked_client_frame(WebSocketOpcode::kBinary, "abc", false));
    fragmented_too_large.append(
        build_masked_client_frame(WebSocketOpcode::kContinuation, "de", true));
    EXPECT_FALSE(
        parser.parse(&fragmented_too_large, &events, &close_code, &error));
    EXPECT_EQ(close_code,
              static_cast<uint16_t>(WebSocketCloseCode::kMessageTooLarge));
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

TEST(WebSocketFrameTest, RejectsClosePayloadWithSingleByte) {
    WebSocketFrameParser parser;
    znet::Buffer buffer;
    std::string payload(1, '\x01');
    buffer.append(build_masked_client_frame(WebSocketOpcode::kClose, payload));

    std::vector<WebSocketFrameEvent> events;
    uint16_t close_code = 0;
    std::string error;
    const bool ok = parser.parse(&buffer, &events, &close_code, &error);

    EXPECT_FALSE(ok);
    EXPECT_EQ(close_code,
              static_cast<uint16_t>(WebSocketCloseCode::kProtocolError));
    EXPECT_NE(error.find("close payload"), std::string::npos);
}

TEST(WebSocketFrameTest, ParsesClosePingAndPongEvents) {
    WebSocketFrameParser parser;
    znet::Buffer buffer;

    std::string close_payload;
    close_payload.push_back(static_cast<char>(0x03));
    close_payload.push_back(static_cast<char>(0xE8)); // 1000
    close_payload.append("bye");

    buffer.append(
        build_masked_client_frame(WebSocketOpcode::kPing, "heartbeat"));
    buffer.append(build_masked_client_frame(WebSocketOpcode::kPong, "ack"));
    buffer.append(
        build_masked_client_frame(WebSocketOpcode::kClose, close_payload));

    std::vector<WebSocketFrameEvent> events;
    uint16_t close_code = 0;
    std::string error;
    const bool ok = parser.parse(&buffer, &events, &close_code, &error);

    EXPECT_TRUE(ok);
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0].opcode, WebSocketOpcode::kPing);
    EXPECT_EQ(events[1].opcode, WebSocketOpcode::kPong);
    EXPECT_EQ(events[2].opcode, WebSocketOpcode::kClose);
    EXPECT_EQ(events[2].close_code,
              static_cast<uint16_t>(WebSocketCloseCode::kNormalClosure));
    EXPECT_EQ(events[2].payload, "bye");
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

TEST(WebSocketFrameTest, EncodesExtendedLengthsAndFinBit) {
    std::string frame_126;
    ASSERT_TRUE(build_websocket_frame(
        WebSocketOpcode::kBinary, std::string(126, 'x'), &frame_126, false));
    ASSERT_GE(frame_126.size(), 4u + 126u);
    EXPECT_EQ(static_cast<uint8_t>(frame_126[0]), 0x02);
    EXPECT_EQ(static_cast<uint8_t>(frame_126[1]), 126);

    std::string frame_127;
    ASSERT_TRUE(build_websocket_frame(
        WebSocketOpcode::kBinary, std::string(66000, 'y'), &frame_127, true));
    ASSERT_GE(frame_127.size(), 10u + 66000u);
    EXPECT_EQ(static_cast<uint8_t>(frame_127[0]), 0x82);
    EXPECT_EQ(static_cast<uint8_t>(frame_127[1]), 127);
}

TEST(WebSocketFrameTest, RejectsInvalidBuildInputs) {
    EXPECT_FALSE(
        build_websocket_frame(WebSocketOpcode::kText, "abc", nullptr, true));

    std::string frame;
    EXPECT_FALSE(build_websocket_frame(WebSocketOpcode::kPing,
                                       std::string(126, 'x'), &frame, true));
    EXPECT_FALSE(build_websocket_frame(WebSocketOpcode::kClose,
                                       std::string(126, 'x'), &frame, true));
}

} // namespace
} // namespace zhttp

int main(int argc, char **argv) {
    zhttp::init_logger();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
