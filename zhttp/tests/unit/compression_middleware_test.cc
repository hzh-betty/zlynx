#include "compression_middleware.h"
#include "zhttp_logger.h"

#include <gtest/gtest.h>
#include <zlib.h>

#ifdef ZHTTP_USE_BROTLI
#include <brotli/decode.h>
#endif

using namespace zhttp;

namespace {

std::string gzip_decompress_for_test(const std::string &data) {
  if (data.empty()) {
    return {};
  }

  z_stream stream;
  stream.zalloc = Z_NULL;
  stream.zfree = Z_NULL;
  stream.opaque = Z_NULL;
  stream.avail_in = 0;
  stream.next_in = Z_NULL;

  if (inflateInit2(&stream, 15 + 16) != Z_OK) {
    return {};
  }

  stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.data()));
  stream.avail_in = static_cast<uInt>(data.size());

  std::string output;
  output.resize(data.size() * 4 + 256);

  while (true) {
    size_t old_size = output.size();
    stream.next_out = reinterpret_cast<Bytef *>(&output[0]) + stream.total_out;
    stream.avail_out = static_cast<uInt>(old_size - stream.total_out);

    int rc = inflate(&stream, Z_NO_FLUSH);
    if (rc == Z_STREAM_END) {
      output.resize(static_cast<size_t>(stream.total_out));
      inflateEnd(&stream);
      return output;
    }
    if (rc != Z_OK) {
      inflateEnd(&stream);
      return {};
    }

    if (stream.avail_out == 0) {
      output.resize(old_size * 2);
    }
  }
}

#ifdef ZHTTP_USE_BROTLI
std::string brotli_decompress_for_test(const std::string &data) {
  if (data.empty()) {
    return {};
  }

  std::string output;
  output.resize(data.size() * 6 + 256);
  size_t output_size = output.size();

  BrotliDecoderResult rc = BrotliDecoderDecompress(
      data.size(),
      reinterpret_cast<const uint8_t *>(data.data()),
      &output_size,
      reinterpret_cast<uint8_t *>(&output[0]));

  if (rc != BROTLI_DECODER_RESULT_SUCCESS) {
    return {};
  }

  output.resize(output_size);
  return output;
}
#endif

std::string large_text_payload() {
  std::string payload;
  for (int i = 0; i < 2048; ++i) {
    payload += "zhttp-compression-test-line-";
    payload += std::to_string(i % 10);
    payload += "\n";
  }
  return payload;
}

} // namespace

TEST(CompressionMiddlewareTest, GzipCompressWhenClientSupportsGzip) {
  CompressionMiddleware::Options opt;
  opt.enable_gzip = true;
  opt.enable_br = false;
  opt.min_compress_size = 32;

  CompressionMiddleware middleware(opt);

  auto req = std::make_shared<HttpRequest>();
  req->set_method(HttpMethod::GET);
  req->set_header("Accept-Encoding", "gzip");

  HttpResponse resp;
  const std::string plain = large_text_payload();
  resp.status(HttpStatus::OK)
      .content_type("text/plain; charset=utf-8")
      .body(plain);

  EXPECT_TRUE(middleware.before(req, resp));
  middleware.after(req, resp);

  EXPECT_EQ(resp.headers().at("Content-Encoding"), "gzip");
  EXPECT_EQ(resp.headers().at("Vary"), "Accept-Encoding");

  const std::string decompressed = gzip_decompress_for_test(resp.body_content());
  EXPECT_EQ(decompressed, plain);
}

TEST(CompressionMiddlewareTest, SkipCompressionWhenBodyTooSmall) {
  CompressionMiddleware::Options opt;
  opt.enable_gzip = true;
  opt.enable_br = false;
  opt.min_compress_size = 1024;

  CompressionMiddleware middleware(opt);

  auto req = std::make_shared<HttpRequest>();
  req->set_method(HttpMethod::GET);
  req->set_header("Accept-Encoding", "gzip");

  HttpResponse resp;
  resp.status(HttpStatus::OK)
      .content_type("text/plain")
      .body("small");

  middleware.after(req, resp);

  EXPECT_EQ(resp.headers().find("Content-Encoding"), resp.headers().end());
  EXPECT_EQ(resp.body_content(), "small");
}

#ifdef ZHTTP_USE_BROTLI
TEST(CompressionMiddlewareTest, PreferBrotliWhenClientSupportsBoth) {
  CompressionMiddleware::Options opt;
  opt.enable_br = true;
  opt.enable_gzip = true;
  opt.min_compress_size = 32;

  CompressionMiddleware middleware(opt);

  auto req = std::make_shared<HttpRequest>();
  req->set_method(HttpMethod::GET);
  req->set_header("Accept-Encoding", "gzip, br");

  HttpResponse resp;
  const std::string plain = large_text_payload();
  resp.status(HttpStatus::OK)
      .content_type("application/json")
      .body(plain);

  middleware.after(req, resp);

  EXPECT_EQ(resp.headers().at("Content-Encoding"), "br");
  const std::string decompressed = brotli_decompress_for_test(resp.body_content());
  EXPECT_EQ(decompressed, plain);
}
#endif

int main(int argc, char **argv) {
  zhttp::init_logger();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
