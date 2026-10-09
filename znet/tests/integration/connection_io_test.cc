#include "../support/network_fixture.h"
#include "zco/sync/event.h"
#include <gtest/gtest.h>
#include <thread>

using namespace znet;
using namespace std::chrono_literals;

TEST(ConnectionIoTest, IncrementalInputEofAndResourceClosure) {
    test::NetworkPair pair;
    ByteBuffer input(2);
    input.append("old");
    ASSERT_EQ(::send(pair.peer->native_handle(), "abcdef", 6, MSG_NOSIGNAL), 6);
    auto first = pair.local->read(input, 2);
    ASSERT_TRUE(first);
    EXPECT_EQ(first.bytes, 2u);
    EXPECT_EQ(input.retrieve_all_as_string(), "oldab");
    EXPECT_EQ(pair.local->read(input, 4).bytes, 4u);
    EXPECT_EQ(input.retrieve_all_as_string(), "cdef");
    ASSERT_TRUE(pair.peer->shutdown_write());
    EXPECT_TRUE(pair.local->read(input).eof);
    EXPECT_TRUE(pair.local->send("response"));
    EXPECT_EQ(test::receive(pair.peer->native_handle(), 8), "response");
    const int owned = pair.local->native_handle();
    EXPECT_TRUE(pair.local->shutdown());
    EXPECT_EQ(pair.local->native_handle(), -1);
    EXPECT_EQ(::fcntl(owned, F_GETFD), -1);
    EXPECT_TRUE(pair.local->close());
}

TEST(ConnectionIoTest, WaitingReadDoesNotBlockSendOnTheSameWorker) {
    test::NetworkPair pair;
    zco::Event entered(true), sent(true);
    ByteBuffer input;
    auto reader = pair.runtime.spawn([&] {
        entered.signal();
        auto read = pair.local->read(input, 4, zco::Deadline::after(1s));
        EXPECT_TRUE(read);
        EXPECT_EQ(read.bytes, 4u);
    });
    auto writer = pair.runtime.spawn([&] {
        entered.wait().value();
        EXPECT_TRUE(pair.local->send("out", 100ms));
        sent.signal();
    });
    ASSERT_TRUE(reader);
    ASSERT_TRUE(writer);
    EXPECT_TRUE(sent.wait(zco::Deadline::after(500ms)));
    EXPECT_EQ(test::receive(pair.peer->native_handle(), 3), "out");
    EXPECT_EQ(::send(pair.peer->native_handle(), "ping", 4, MSG_NOSIGNAL), 4);
    EXPECT_TRUE(writer.value().join());
    EXPECT_TRUE(reader.value().join());
    EXPECT_EQ(input.view(), "ping");
}

TEST(ConnectionIoTest, ConcurrentThreadSendsKeepEachPayloadContiguous) {
    test::NetworkPair pair;
    constexpr size_t rounds = 64;
    const std::string a(64, 'A'), b(64, 'B');
    std::thread first([&] {
        for (size_t i = 0; i < rounds; ++i)
            EXPECT_TRUE(pair.local->send(a));
    });
    std::thread second([&] {
        for (size_t i = 0; i < rounds; ++i)
            EXPECT_TRUE(pair.local->send(b));
    });
    const auto received =
        test::receive(pair.peer->native_handle(), rounds * 128);
    first.join();
    second.join();
    ASSERT_EQ(received.size(), rounds * 128);
    for (size_t offset = 0; offset < received.size(); offset += 64)
        EXPECT_EQ(received.substr(offset, 64), received[offset] == 'A' ? a : b);
}

TEST(ConnectionIoTest,
     CloseInterruptsInfiniteReadAndRemainsUsableAfterRuntimeStops) {
    test::NetworkPair pair;
    ByteBuffer input;
    zco::Event entered(true);
    auto reader = pair.runtime.spawn([&] {
        entered.signal();
        auto result = pair.local->read(input);
        EXPECT_FALSE(result);
        EXPECT_EQ(result.error.code, std::errc::bad_file_descriptor);
    });
    ASSERT_TRUE(reader);
    entered.wait().value();
    // A barrier on this worker runs only after the reader has parked.
    auto barrier = pair.runtime.executor(0).spawn([] {});
    ASSERT_TRUE(barrier);
    barrier.value().join().value();
    EXPECT_TRUE(pair.local->close());
    EXPECT_TRUE(reader.value().join());
    pair.runtime.request_stop();
    pair.runtime.join();
    EXPECT_TRUE(pair.local->close());
    EXPECT_EQ(pair.local->state(), Connection::State::closed);
}

TEST(ConnectionIoTest, WriteTimeoutReportsPartialProgressUnderBackpressure) {
    test::NetworkPair pair(20ms);
    const int size = 1024;
    ASSERT_EQ(::setsockopt(pair.local->native_handle(), SOL_SOCKET, SO_SNDBUF,
                           &size, sizeof(size)),
              0);
    const auto started = std::chrono::steady_clock::now();
    const std::string payload(1024 * 1024, 'x');
    auto result = pair.local->send(payload);
    ASSERT_FALSE(result);
    EXPECT_GT(result.bytes, 0u);
    EXPECT_LT(result.bytes, payload.size());
    EXPECT_EQ(result.error.code, std::errc::timed_out);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 500ms);
    EXPECT_EQ(test::receive(pair.peer->native_handle(), result.bytes),
              payload.substr(0, result.bytes));
    EXPECT_FALSE(pair.local->connected());
}

TEST(ConnectionIoTest, ApplicationDeadlineDoesNotResetBetweenReads) {
    test::NetworkPair pair;
    ByteBuffer input;
    pair.local->set_read_deadline(zco::Deadline::after(30ms));
    ASSERT_EQ(::send(pair.peer->native_handle(), "x", 1, MSG_NOSIGNAL), 1);
    EXPECT_TRUE(pair.local->read(input, 1));
    auto result = pair.local->read(input, 1, zco::Deadline::after(1s));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error.code, std::errc::timed_out);
    EXPECT_TRUE(pair.local->read_deadline_expired());
    EXPECT_EQ(input.view(), "x");
    pair.local->set_read_deadline({});
    ASSERT_EQ(::send(pair.peer->native_handle(), "y", 1, MSG_NOSIGNAL), 1);
    EXPECT_TRUE(pair.local->read(input, 1));
    EXPECT_EQ(input.view(), "xy");
}
