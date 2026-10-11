#include "protocol/http/http_response_writer.h"
#include "zco/coroutine.h"
#include "znet/server/connection.h"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <gtest/gtest.h>
#include <unistd.h>

using namespace zhttp;
namespace {
struct Output {
    std::string bytes;
    std::size_t writes = 0;
    std::size_t fail_after = 0;
};

class PartialWriteStream final : public znet::ByteStream {
  public:
    explicit PartialWriteStream(std::shared_ptr<Output> output)
        : output_(std::move(output)) {}
    znet::Result<void> start(zco::Deadline) override { return {}; }
    znet::Transfer read_some(void *, std::size_t, zco::Deadline) override {
        return {0, {}, true};
    }
    znet::Transfer write_some(const void *bytes, std::size_t size,
                             zco::Deadline) override {
        // 强制另一协程复用同一共享栈，再恢复写出协程并消费其借用缓冲区。
        auto scratch = zco::current_executor().spawn([] {
            volatile char scratch_bytes[16384];
            for (std::size_t i = 0; i < sizeof(scratch_bytes); ++i)
                scratch_bytes[i] = 'x';
            zco::yield();
        }).value();
        scratch.join().value();
        const auto count = std::min(size, std::size_t{257});
        output_->bytes.append(static_cast<const char *>(bytes), count);
        if (++output_->writes == output_->fail_after)
            return {count, znet::io_error("write", EPIPE), false};
        return {count, {}, false};
    }
    znet::Result<void> shutdown_write(zco::Deadline) override { return {}; }
    znet::Result<void> close() override { return {}; }
    znet::Result<znet::Endpoint> local_endpoint() const override {
        return znet::Endpoint::ipv4("127.0.0.1", 1);
    }
    znet::Result<znet::Endpoint> remote_endpoint() const override {
        return znet::Endpoint::ipv4("127.0.0.1", 2);
    }
    int native_handle() const override { return -1; }

  private:
    std::shared_ptr<Output> output_;
};

std::string payload() {
    std::string bytes(2 * 8192 + 521, '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<char>((i * 31 + i / 257) % 256);
    return bytes;
}

class HttpResponseWriterTest : public ::testing::Test {
  protected:
    void SetUp() override {
        zco::RuntimeOptions options;
        options.worker_count = 1;
        options.shared_stack_count = 1;
        runtime_ = std::make_unique<zco::Runtime>(options);
        output_ = std::make_shared<Output>();
        connection_ = std::make_shared<znet::Connection>(
            std::make_unique<PartialWriteStream>(output_), runtime_->executor(0));
        ASSERT_TRUE(connection_->start());
    }
    WriteResult write(HttpContext &context) {
        auto result = WriteResult::Failed;
        auto task = runtime_->spawn([&] {
            result = HttpResponseWriter::send(connection_, context);
        }).value();
        task.join().value();
        return result;
    }
    std::unique_ptr<zco::Runtime> runtime_;
    std::shared_ptr<Output> output_;
    std::shared_ptr<znet::Connection> connection_;
};

TEST_F(HttpResponseWriterTest, KnownLengthStreamSurvivesPartialWritesAndStackReuse) {
    const auto bytes = payload();
    std::size_t offset = 0;
    auto request = std::make_shared<HttpRequest>();
    HttpContext context(request);
    context.response().stream([&](char *buffer, std::size_t capacity) {
        const auto count = std::min(capacity, bytes.size() - offset);
        std::memcpy(buffer, bytes.data() + offset, count);
        offset += count;
        return count;
    }, bytes.size());
    EXPECT_EQ(write(context), WriteResult::Completed);
    const auto body = output_->bytes.find("\r\n\r\n");
    ASSERT_NE(body, std::string::npos);
    EXPECT_EQ(output_->bytes.substr(body + 4), bytes);
    EXPECT_NE(output_->bytes.substr(0, body).find(
                  "Content-Length: " + std::to_string(bytes.size())),
              std::string::npos);
    EXPECT_EQ(output_->bytes.substr(0, body).find("Transfer-Encoding"),
              std::string::npos);
    EXPECT_GT(output_->writes, 64u);
}

TEST_F(HttpResponseWriterTest, FileRangeSurvivesPartialWritesAndStackReuse) {
    const auto bytes = payload();
    struct TemporaryFile {
        char path[64] = "/tmp/zlynx-writer-view-XXXXXX";
        int fd = ::mkstemp(path);
        ~TemporaryFile() {
            if (fd >= 0)
                ::close(fd);
            ::unlink(path);
        }
    } file;
    ASSERT_GE(file.fd, 0);
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::write(file.fd, bytes.data() + offset, bytes.size() - offset);
        ASSERT_GT(count, 0);
        offset += count;
    }
    auto request = std::make_shared<HttpRequest>();
    HttpContext context(request);
    context.response().body(HttpBody::file(file.path, 37, bytes.size() - 91));
    ASSERT_EQ(::unlink(file.path), 0);
    EXPECT_EQ(write(context), WriteResult::Completed);
    const auto body = output_->bytes.find("\r\n\r\n");
    ASSERT_NE(body, std::string::npos);
    EXPECT_EQ(output_->bytes.substr(body + 4), bytes.substr(37, bytes.size() - 91));
}

TEST_F(HttpResponseWriterTest, UnknownLengthHttp10StreamUsesCloseDelimitedBody) {
    const auto bytes = payload();
    std::size_t offset = 0;
    auto request = std::make_shared<HttpRequest>();
    request->set_version(HttpVersion::HTTP_1_0);
    HttpContext context(request);
    context.response().stream([&](char *buffer, std::size_t capacity) {
        const auto count = std::min(capacity, bytes.size() - offset);
        std::memcpy(buffer, bytes.data() + offset, count);
        offset += count;
        return count;
    });
    EXPECT_EQ(write(context), WriteResult::Completed);
    const auto body = output_->bytes.find("\r\n\r\n");
    ASSERT_NE(body, std::string::npos);
    EXPECT_EQ(output_->bytes.substr(body + 4), bytes);
    const auto headers = output_->bytes.substr(0, body);
    EXPECT_EQ(headers.find("Content-Length"), std::string::npos);
    EXPECT_EQ(headers.find("Transfer-Encoding"), std::string::npos);
    EXPECT_NE(headers.find("Connection: close"), std::string::npos);
}

TEST_F(HttpResponseWriterTest, StreamFailurePreservesPartialProgress) {
    const auto bytes = payload();
    std::size_t offset = 0;
    auto request = std::make_shared<HttpRequest>();
    HttpContext context(request);
    context.response().stream([&](char *buffer, std::size_t capacity) {
        const auto count = std::min(capacity, bytes.size() - offset);
        std::memcpy(buffer, bytes.data() + offset, count);
        offset += count;
        return count;
    }, bytes.size());
    output_->fail_after = 3;
    EXPECT_EQ(write(context), WriteResult::Failed);
    EXPECT_TRUE(context.response().committed());
    EXPECT_FALSE(connection_->connected());
    const auto body = output_->bytes.find("\r\n\r\n");
    ASSERT_NE(body, std::string::npos);
    const auto received = output_->bytes.substr(body + 4);
    EXPECT_FALSE(received.empty());
    EXPECT_LT(received.size(), bytes.size());
    EXPECT_EQ(received, bytes.substr(0, received.size()));
}
} // namespace
