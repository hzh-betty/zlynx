#include "zhttp/mid/compression_middleware.h"
#include "zhttp/zhttp_logger.h"

#include <brotli/decode.h>
#include <gtest/gtest.h>
#include <zlib.h>

using namespace zhttp;
using namespace zhttp::mid;

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
        stream.next_out =
            reinterpret_cast<Bytef *>(&output[0]) + stream.total_out;
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

std::string brotli_decompress_for_test(const std::string &data,
                                       size_t expected_size) {
    if (data.empty()) {
        return {};
    }

    size_t output_size = expected_size + 64;
    for (int i = 0; i < 4; ++i) {
        std::string output;
        output.resize(output_size);

        size_t decoded_size = output.size();
        BrotliDecoderResult rc = BrotliDecoderDecompress(
            data.size(), reinterpret_cast<const uint8_t *>(data.data()),
            &decoded_size, reinterpret_cast<uint8_t *>(&output[0]));

        if (rc == BROTLI_DECODER_RESULT_SUCCESS) {
            output.resize(decoded_size);
            return output;
        }
        if (rc != BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) {
            return {};
        }

        output_size *= 2;
    }

    return {};
}

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

    const std::string decompressed =
        gzip_decompress_for_test(resp.body_content());
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
    resp.status(HttpStatus::OK).content_type("text/plain").body("small");

    middleware.after(req, resp);

    EXPECT_EQ(resp.headers().find("Content-Encoding"), resp.headers().end());
    EXPECT_EQ(resp.body_content(), "small");
}

TEST(CompressionMiddlewareTest, SkipCompressionForChunkedResponse) {
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
        .body(plain)
        .enable_chunked();

    middleware.after(req, resp);

    EXPECT_EQ(resp.headers().find("Content-Encoding"), resp.headers().end());
    EXPECT_EQ(resp.headers().find("Content-Length"), resp.headers().end());
    EXPECT_EQ(resp.body_content(), plain);
}

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
    resp.status(HttpStatus::OK).content_type("application/json").body(plain);

    middleware.after(req, resp);

    EXPECT_EQ(resp.headers().at("Content-Encoding"), "br");
    const std::string decompressed =
        brotli_decompress_for_test(resp.body_content(), plain.size());
    EXPECT_EQ(decompressed, plain);
}

TEST(CompressionMiddlewareTest, SkipCompressionForHeadRequest) {
    CompressionMiddleware::Options opt;
    opt.enable_gzip = true;
    opt.enable_br = false;
    opt.min_compress_size = 32;

    CompressionMiddleware middleware(opt);

    auto req = std::make_shared<HttpRequest>();
    req->set_method(HttpMethod::HEAD);
    req->set_header("Accept-Encoding", "gzip");

    HttpResponse resp;
    const std::string plain = large_text_payload();
    resp.status(HttpStatus::OK).content_type("text/plain").body(plain);

    middleware.after(req, resp);
    EXPECT_EQ(resp.headers().count("Content-Encoding"), 0U);
    EXPECT_EQ(resp.body_content(), plain);
}

TEST(CompressionMiddlewareTest,
     SkipCompressionForPreEncodedOrStreamingResponse) {
    CompressionMiddleware::Options opt;
    opt.enable_gzip = true;
    opt.enable_br = false;
    opt.min_compress_size = 32;
    CompressionMiddleware middleware(opt);

    auto req = std::make_shared<HttpRequest>();
    req->set_method(HttpMethod::GET);
    req->set_header("Accept-Encoding", "gzip");
    const std::string plain = large_text_payload();

    {
        HttpResponse already_encoded;
        already_encoded.status(HttpStatus::OK)
            .content_type("text/plain")
            .header("Content-Encoding", "br")
            .body(plain);
        middleware.after(req, already_encoded);
        EXPECT_EQ(already_encoded.headers().at("Content-Encoding"), "br");
    }

    {
        HttpResponse streaming;
        streaming.status(HttpStatus::OK)
            .content_type("text/plain")
            .body(plain)
            .stream([](char *, size_t) { return 0U; });
        middleware.after(req, streaming);
        EXPECT_EQ(streaming.headers().count("Content-Encoding"), 0U);
    }
}

TEST(CompressionMiddlewareTest,
     GzipNegotiationSupportsQValuesAndIgnoresNonExactTokens) {
    CompressionMiddleware::Options opt;
    opt.enable_gzip = true;
    opt.enable_br = false;
    opt.min_compress_size = 32;
    opt.gzip_level = 99;

    CompressionMiddleware middleware(opt);

    auto req = std::make_shared<HttpRequest>();
    req->set_method(HttpMethod::GET);
    req->set_header("Accept-Encoding", "x-gzip, gzip;q=0.7");

    HttpResponse resp;
    const std::string plain = large_text_payload();
    resp.status(HttpStatus::OK).content_type("text/plain").body(plain);

    middleware.after(req, resp);
    EXPECT_EQ(resp.headers().at("Content-Encoding"), "gzip");
    EXPECT_EQ(gzip_decompress_for_test(resp.body_content()), plain);
}

TEST(CompressionMiddlewareTest,
     CanCompressErrorResponsesWhenOnlySuccessConstraintDisabled) {
    CompressionMiddleware::Options opt;
    opt.enable_gzip = true;
    opt.enable_br = false;
    opt.only_compress_success_response = false;
    opt.min_compress_size = 32;
    opt.gzip_level = -5;

    CompressionMiddleware middleware(opt);
    auto req = std::make_shared<HttpRequest>();
    req->set_method(HttpMethod::GET);
    req->set_header("Accept-Encoding", "gzip");

    HttpResponse resp;
    const std::string plain = large_text_payload();
    resp.status(HttpStatus::INTERNAL_SERVER_ERROR)
        .content_type("text/plain")
        .body(plain);

    middleware.after(req, resp);
    EXPECT_EQ(resp.headers().at("Content-Encoding"), "gzip");
    EXPECT_EQ(gzip_decompress_for_test(resp.body_content()), plain);
}

TEST(CompressionMiddlewareTest,
     SkipCompressionWhenTypeNotCompressibleAndAllowMissingTypeByDefault) {
    CompressionMiddleware::Options opt;
    opt.enable_gzip = true;
    opt.enable_br = false;
    opt.min_compress_size = 32;
    CompressionMiddleware middleware(opt);

    auto req = std::make_shared<HttpRequest>();
    req->set_method(HttpMethod::GET);
    req->set_header("Accept-Encoding", "gzip");
    const std::string plain = large_text_payload();

    {
        HttpResponse image_resp;
        image_resp.status(HttpStatus::OK).content_type("image/png").body(plain);
        middleware.after(req, image_resp);
        EXPECT_EQ(image_resp.headers().count("Content-Encoding"), 0U);
    }

    {
        HttpResponse no_type_resp;
        no_type_resp.status(HttpStatus::OK).body(plain);
        middleware.after(req, no_type_resp);
        EXPECT_EQ(no_type_resp.headers().at("Content-Encoding"), "gzip");
    }
}

TEST(CompressionMiddlewareTest, VaryHeaderIsAppendedOrKeptWithoutDuplicate) {
    CompressionMiddleware::Options opt;
    opt.enable_gzip = true;
    opt.enable_br = false;
    opt.min_compress_size = 32;
    CompressionMiddleware middleware(opt);

    auto req = std::make_shared<HttpRequest>();
    req->set_method(HttpMethod::GET);
    req->set_header("Accept-Encoding", "gzip");
    const std::string plain = large_text_payload();

    {
        HttpResponse resp;
        resp.status(HttpStatus::OK)
            .content_type("text/plain")
            .header("Vary", "Origin")
            .body(plain);
        middleware.after(req, resp);
        EXPECT_EQ(resp.headers().at("Vary"), "Origin, Accept-Encoding");
    }

    {
        HttpResponse resp;
        resp.status(HttpStatus::OK)
            .content_type("text/plain")
            .header("Vary", "origin, ACCEPT-ENCODING")
            .body(plain);
        middleware.after(req, resp);
        EXPECT_EQ(resp.headers().at("Vary"), "origin, ACCEPT-ENCODING");
    }
}


TEST(CompressionMiddlewareTest, HonorsEncodingWeightsExclusionsAndWildcard) {
    struct Case { const char *header; const char *encoding; };
    const Case cases[] = {
        {"gzip;q=0, br;q=0", ""},
        {"br;q=0.2, gzip;q=0.9", "gzip"},
        {"br;q=0.9, gzip;q=0.2", "br"},
        {"br;q=0.5, gzip;q=0.5", "br"},
        {"*;q=0.7, br;q=0", "gzip"},
        {"*;q=0, gzip;q=0.5", "gzip"},
        {"identity;q=1, gzip;q=0.5", ""},
        {" BR ; Q=0, GZip ; q=1.000", "gzip"},
        {"x-gzip, notbr", ""},
        {"gzip;q=0, gzip;q=1", ""},
        {"br;q=nan, gzip;q=1.5", ""},
        {"*;q=0, identity;q=0.5", ""},

    };
    CompressionMiddleware middleware;
    for (const auto &item : cases) {
        SCOPED_TRACE(item.header);
        auto request = std::make_shared<HttpRequest>();
        request->set_method(HttpMethod::GET);
        request->set_header("Accept-Encoding", item.header);
        HttpResponse response;
        const std::string plain(4096, 'a');
        response.text(plain);
        middleware.after(request, response);
        EXPECT_EQ(response.status_code(), HttpStatus::OK);
        auto it = response.headers().find("Content-Encoding");
        EXPECT_EQ(it == response.headers().end() ? "" : it->second, item.encoding);
        if (std::string(item.encoding) == "gzip") {
            EXPECT_EQ(gzip_decompress_for_test(response.body_content()), plain);
        } else if (std::string(item.encoding) == "br") {
            EXPECT_EQ(brotli_decompress_for_test(response.body_content(), plain.size()), plain);
        } else {
            EXPECT_EQ(response.body_content(), plain);
        }
        EXPECT_EQ(response.headers().at("Vary"), "Accept-Encoding");
    }
}

TEST(CompressionMiddlewareTest, RejectsWhenAllAvailableRepresentationsAreExcluded) {
    for (const char *header : {"*;q=0", "gzip;q=0, br;q=0, identity;q=0"}) {
        auto request = std::make_shared<HttpRequest>();
        request->set_method(HttpMethod::GET);
        request->set_header("Accept-Encoding", header);
        HttpResponse response;
        response.text(std::string(4096, 'a'));
        CompressionMiddleware middleware;
        middleware.after(request, response);
        EXPECT_EQ(response.status_code(), HttpStatus::NOT_ACCEPTABLE);
        EXPECT_TRUE(response.body_content().empty());
        EXPECT_EQ(response.headers().at("Content-Length"), "0");
    }
}

TEST(CompressionMiddlewareTest, VaryUsesExactTokensAndPreservesWildcard) {
    auto request = std::make_shared<HttpRequest>();
    request->set_header("Accept-Encoding", "gzip");
    CompressionMiddleware middleware;

    for (const char *vary : {"X-Accept-Encoding", "Origin,  ACCEPT-ENCODING ", "*"}) {
        HttpResponse response;
        response.text(std::string(4096, 'a')).header("Vary", vary);
        middleware.after(request, response);
        ASSERT_EQ(response.headers().at("Content-Encoding"), "gzip");
        EXPECT_EQ(response.headers().at("Vary"),
                  std::string(vary) == "X-Accept-Encoding"
                      ? "X-Accept-Encoding, Accept-Encoding"
                      : vary);
    }
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    zhttp::init_logger();
    return RUN_ALL_TESTS();
}
