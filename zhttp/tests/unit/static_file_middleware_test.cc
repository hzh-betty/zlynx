#include "http_request.h"
#include "http_response.h"
#include "static_file_middleware.h"
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

} // namespace

TEST(StaticFileMiddlewareTest, PreferBrWhenClientSupportsBrAndFileExists) {
  TempDir dir;
  dir.write_file("app.js", "console.log('plain');");
  dir.write_file("app.js.br", "brotli-data");
  dir.write_file("app.js.gz", "gzip-data");

  StaticFileMiddleware::Options opt;
  opt.uri_prefix = "/static";
  opt.document_root = dir.path();
  opt.enable_memory_cache = false;

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

TEST(StaticFileMiddlewareTest, ReturnNotModifiedWhenIfModifiedSinceMatches) {
  TempDir dir;
  dir.write_file("hello.txt", "hello");

  StaticFileMiddleware::Options opt;
  opt.uri_prefix = "/assets";
  opt.document_root = dir.path();
  opt.enable_memory_cache = false;

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

TEST(StaticFileMiddlewareTest, ServeFromMemoryCacheWithinTtl) {
  TempDir dir;
  const std::string file_path = dir.write_file("cache.txt", "v1");

  StaticFileMiddleware::Options opt;
  opt.uri_prefix = "/assets";
  opt.document_root = dir.path();
  opt.enable_memory_cache = true;
  opt.memory_cache_time = 10;

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

int main(int argc, char **argv) {
  zhttp::init_logger();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
