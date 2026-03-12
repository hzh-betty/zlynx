#include "http_request.h"
#include "http_response.h"
#include "static_file_opt.h"
#include "zhttp_logger.h"

#include <cstdio>
#include <fstream>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace zhttp;

namespace {

class TempDir {
public:
  TempDir() {
    char tmpl[] = "/tmp/zhttp-static-mw-XXXXXX";
    char *result = ::mkdtemp(tmpl);
    if (!result) {
      throw std::runtime_error("failed to create temp dir");
    }
    path_ = result;
  }

  ~TempDir() {
    for (const auto &file : files_) {
      ::unlink(file.c_str());
    }
    if (!path_.empty()) {
      ::rmdir(path_.c_str());
    }
  }

  const std::string &path() const { return path_; }

  std::string write_file(const std::string &name, const std::string &content) {
    std::string file = path_ + "/" + name;
    std::ofstream ofs(file, std::ios::out | std::ios::binary);
    ofs << content;
    ofs.close();
    files_.push_back(file);
    return file;
  }

private:
  std::string path_;
  std::vector<std::string> files_;
};

HttpRequest::ptr make_request(HttpMethod method, const std::string &path) {
  auto req = std::make_shared<HttpRequest>();
  req->set_method(method);
  req->set_path(path);
  req->set_version(HttpVersion::HTTP_1_1);
  req->set_header("Connection", "keep-alive");
  return req;
}

class StaticFileMiddlewareTest : public ::testing::Test {
protected:
  StaticFileMiddleware::Options make_options(const std::string &uri_prefix,
                                             const std::string &document_root,
                                             bool enable_memory_cache = false,
                                             int memory_cache_time = 0) {
    StaticFileMiddleware::Options opt;
    opt.uri_prefix = uri_prefix;
    opt.document_root = document_root;
    opt.enable_memory_cache = enable_memory_cache;
    opt.memory_cache_time = memory_cache_time;
    return opt;
  }
};

} // namespace

TEST_F(StaticFileMiddlewareTest, PreferBrWhenClientSupportsBrAndFileExists) {
  TempDir dir;
  dir.write_file("app.js", "console.log('plain');");
  dir.write_file("app.js.br", "brotli-data");
  dir.write_file("app.js.gz", "gzip-data");

  StaticFileMiddleware::Options opt = make_options("/static", dir.path(), false);

  StaticFileMiddleware middleware(opt);
  auto req = make_request(HttpMethod::GET, "/static/app.js");
  req->set_header("Accept-Encoding", "gzip, br");
  HttpResponse resp;

  const bool should_continue = middleware.before(req, resp);

  EXPECT_FALSE(should_continue);
  EXPECT_EQ(resp.status_code(), HttpStatus::OK);
  EXPECT_EQ(resp.headers().at("Content-Encoding"), "br");
  EXPECT_EQ(resp.body_content(), "brotli-data");
}

TEST_F(StaticFileMiddlewareTest, ReturnNotModifiedWhenIfModifiedSinceMatches) {
  TempDir dir;
  dir.write_file("hello.txt", "hello");

  StaticFileMiddleware::Options opt = make_options("/assets", dir.path(), false);

  StaticFileMiddleware middleware(opt);

  auto first = make_request(HttpMethod::GET, "/assets/hello.txt");
  HttpResponse first_resp;
  EXPECT_FALSE(middleware.before(first, first_resp));
  ASSERT_NE(first_resp.headers().find("Last-Modified"), first_resp.headers().end());
  const std::string lm = first_resp.headers().at("Last-Modified");

  auto second = make_request(HttpMethod::GET, "/assets/hello.txt");
  second->set_header("If-Modified-Since", lm);
  HttpResponse second_resp;
  const bool should_continue = middleware.before(second, second_resp);

  EXPECT_FALSE(should_continue);
  EXPECT_EQ(second_resp.status_code(), HttpStatus::NOT_MODIFIED);
}

TEST_F(StaticFileMiddlewareTest, ReturnNotModifiedWhenIfNoneMatchMatches) {
  TempDir dir;
  dir.write_file("hello.txt", "hello");

  StaticFileMiddleware::Options opt = make_options("/assets", dir.path(), false);

  StaticFileMiddleware middleware(opt);

  auto first = make_request(HttpMethod::GET, "/assets/hello.txt");
  HttpResponse first_resp;
  EXPECT_FALSE(middleware.before(first, first_resp));
  ASSERT_NE(first_resp.headers().find("ETag"), first_resp.headers().end());
  const std::string etag = first_resp.headers().at("ETag");

  auto second = make_request(HttpMethod::GET, "/assets/hello.txt");
  second->set_header("If-None-Match", etag);
  HttpResponse second_resp;
  const bool should_continue = middleware.before(second, second_resp);

  EXPECT_FALSE(should_continue);
  EXPECT_EQ(second_resp.status_code(), HttpStatus::NOT_MODIFIED);
  ASSERT_NE(second_resp.headers().find("ETag"), second_resp.headers().end());
  EXPECT_EQ(second_resp.headers().at("ETag"), etag);
}

TEST_F(StaticFileMiddlewareTest, IfNoneMatchHasPrecedenceOverIfModifiedSince) {
  TempDir dir;
  dir.write_file("hello.txt", "hello");

  StaticFileMiddleware::Options opt = make_options("/assets", dir.path(), false);

  StaticFileMiddleware middleware(opt);

  auto first = make_request(HttpMethod::GET, "/assets/hello.txt");
  HttpResponse first_resp;
  EXPECT_FALSE(middleware.before(first, first_resp));
  ASSERT_NE(first_resp.headers().find("Last-Modified"), first_resp.headers().end());
  const std::string lm = first_resp.headers().at("Last-Modified");

  auto second = make_request(HttpMethod::GET, "/assets/hello.txt");
  second->set_header("If-None-Match", "W/\"non-match\"");
  second->set_header("If-Modified-Since", lm);
  HttpResponse second_resp;
  const bool should_continue = middleware.before(second, second_resp);

  EXPECT_FALSE(should_continue);
  EXPECT_EQ(second_resp.status_code(), HttpStatus::OK);
  EXPECT_EQ(second_resp.body_content(), "hello");
}

TEST_F(StaticFileMiddlewareTest, ServeFromMemoryCacheWithinTtl) {
  TempDir dir;
  const std::string file_path = dir.write_file("cache.txt", "v1");

  StaticFileMiddleware::Options opt = make_options("/assets", dir.path(), true, 10);

  StaticFileMiddleware middleware(opt);

  auto first = make_request(HttpMethod::GET, "/assets/cache.txt");
  HttpResponse first_resp;
  EXPECT_FALSE(middleware.before(first, first_resp));
  EXPECT_EQ(first_resp.body_content(), "v1");

  ::unlink(file_path.c_str());

  auto second = make_request(HttpMethod::GET, "/assets/cache.txt");
  HttpResponse second_resp;
  const bool should_continue = middleware.before(second, second_resp);

  EXPECT_FALSE(should_continue);
  EXPECT_EQ(second_resp.status_code(), HttpStatus::OK);
  EXPECT_EQ(second_resp.body_content(), "v1");
}

TEST_F(StaticFileMiddlewareTest, ReturnNotModifiedByEtagFromMemoryCache) {
  TempDir dir;
  const std::string file_path = dir.write_file("cache.txt", "v1");

  StaticFileMiddleware::Options opt = make_options("/assets", dir.path(), true, 10);

  StaticFileMiddleware middleware(opt);

  auto first = make_request(HttpMethod::GET, "/assets/cache.txt");
  HttpResponse first_resp;
  EXPECT_FALSE(middleware.before(first, first_resp));
  ASSERT_NE(first_resp.headers().find("ETag"), first_resp.headers().end());
  const std::string etag = first_resp.headers().at("ETag");

  ::unlink(file_path.c_str());

  auto second = make_request(HttpMethod::GET, "/assets/cache.txt");
  second->set_header("If-None-Match", etag);
  HttpResponse second_resp;
  const bool should_continue = middleware.before(second, second_resp);

  EXPECT_FALSE(should_continue);
  EXPECT_EQ(second_resp.status_code(), HttpStatus::NOT_MODIFIED);
  ASSERT_NE(second_resp.headers().find("ETag"), second_resp.headers().end());
  EXPECT_EQ(second_resp.headers().at("ETag"), etag);
  EXPECT_TRUE(second_resp.body_content().empty());
}

TEST_F(StaticFileMiddlewareTest, ServePartialContentWhenRangeIsValid) {
  TempDir dir;
  dir.write_file("range.txt", "0123456789");

  StaticFileMiddleware::Options opt = make_options("/assets", dir.path(), false);

  StaticFileMiddleware middleware(opt);
  auto req = make_request(HttpMethod::GET, "/assets/range.txt");
  req->set_header("Range", "bytes=2-5");
  HttpResponse resp;

  const bool should_continue = middleware.before(req, resp);

  EXPECT_FALSE(should_continue);
  EXPECT_EQ(resp.status_code(), HttpStatus::PARTIAL_CONTENT);
  EXPECT_EQ(resp.headers().at("Accept-Ranges"), "bytes");
  EXPECT_EQ(resp.headers().at("Content-Range"), "bytes 2-5/10");
  EXPECT_EQ(resp.body_content(), "2345");
}

TEST_F(StaticFileMiddlewareTest, Return416WhenRangeNotSatisfiable) {
  TempDir dir;
  dir.write_file("range.txt", "0123456789");

  StaticFileMiddleware::Options opt = make_options("/assets", dir.path(), false);

  StaticFileMiddleware middleware(opt);
  auto req = make_request(HttpMethod::GET, "/assets/range.txt");
  req->set_header("Range", "bytes=100-200");
  HttpResponse resp;

  const bool should_continue = middleware.before(req, resp);

  EXPECT_FALSE(should_continue);
  EXPECT_EQ(resp.status_code(), HttpStatus::REQUESTED_RANGE_NOT_SATISFIABLE);
  EXPECT_EQ(resp.headers().at("Accept-Ranges"), "bytes");
  EXPECT_EQ(resp.headers().at("Content-Range"), "bytes */10");
  EXPECT_TRUE(resp.body_content().empty());
}

TEST_F(StaticFileMiddlewareTest, IgnoreRangeWhenIfRangeDoesNotMatch) {
  TempDir dir;
  dir.write_file("range.txt", "0123456789");

  StaticFileMiddleware::Options opt = make_options("/assets", dir.path(), false);

  StaticFileMiddleware middleware(opt);
  auto req = make_request(HttpMethod::GET, "/assets/range.txt");
  req->set_header("Range", "bytes=2-5");
  req->set_header("If-Range", "Wed, 21 Oct 2015 07:28:00 GMT");
  HttpResponse resp;

  const bool should_continue = middleware.before(req, resp);

  EXPECT_FALSE(should_continue);
  EXPECT_EQ(resp.status_code(), HttpStatus::OK);
  EXPECT_EQ(resp.headers().at("Accept-Ranges"), "bytes");
  EXPECT_EQ(resp.body_content(), "0123456789");
}

TEST_F(StaticFileMiddlewareTest, ServeHeadPartialContentWhenRangeIsValid) {
  TempDir dir;
  dir.write_file("range.txt", "0123456789");

  StaticFileMiddleware::Options opt = make_options("/assets", dir.path(), false);

  StaticFileMiddleware middleware(opt);
  auto req = make_request(HttpMethod::HEAD, "/assets/range.txt");
  req->set_header("Range", "bytes=2-5");
  HttpResponse resp;

  const bool should_continue = middleware.before(req, resp);

  EXPECT_FALSE(should_continue);
  EXPECT_EQ(resp.status_code(), HttpStatus::PARTIAL_CONTENT);
  EXPECT_EQ(resp.headers().at("Accept-Ranges"), "bytes");
  EXPECT_EQ(resp.headers().at("Content-Range"), "bytes 2-5/10");
  EXPECT_EQ(resp.headers().at("Content-Length"), "4");
  EXPECT_TRUE(resp.body_content().empty());
}

TEST_F(StaticFileMiddlewareTest, ServePartialRangeFromMemoryCache) {
  TempDir dir;
  const std::string file_path = dir.write_file("cache-range.txt", "0123456789");

  StaticFileMiddleware::Options opt = make_options("/assets", dir.path(), true, 10);

  StaticFileMiddleware middleware(opt);

  auto warm_req = make_request(HttpMethod::GET, "/assets/cache-range.txt");
  HttpResponse warm_resp;
  EXPECT_FALSE(middleware.before(warm_req, warm_resp));
  EXPECT_EQ(warm_resp.status_code(), HttpStatus::OK);

  ::unlink(file_path.c_str());

  auto req = make_request(HttpMethod::GET, "/assets/cache-range.txt");
  req->set_header("Range", "bytes=3-6");
  HttpResponse resp;

  const bool should_continue = middleware.before(req, resp);

  EXPECT_FALSE(should_continue);
  EXPECT_EQ(resp.status_code(), HttpStatus::PARTIAL_CONTENT);
  EXPECT_EQ(resp.headers().at("Accept-Ranges"), "bytes");
  EXPECT_EQ(resp.headers().at("Content-Range"), "bytes 3-6/10");
  EXPECT_EQ(resp.body_content(), "3456");
}

int main(int argc, char **argv) {
  zhttp::init_logger();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
