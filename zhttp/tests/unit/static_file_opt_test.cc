#include "zhttp/http_request.h"
#include "zhttp/http_response.h"
#include "zhttp/mid/static_file_middleware.h"
#include "zhttp/zhttp_logger.h"

#include <cstdio>
#include <fstream>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace zhttp;
using namespace zhttp::mid;

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
        for (auto it = dirs_.rbegin(); it != dirs_.rend(); ++it) {
            ::rmdir(it->c_str());
        }
        if (!path_.empty()) {
            ::rmdir(path_.c_str());
        }
    }

    const std::string &path() const { return path_; }

    std::string write_file(const std::string &name,
                           const std::string &content) {
        std::string file = path_ + "/" + name;
        std::ofstream ofs(file, std::ios::out | std::ios::binary);
        ofs << content;
        ofs.close();
        files_.push_back(file);
        return file;
    }

    std::string make_dir(const std::string &name) {
        std::string dir = path_ + "/" + name;
        if (::mkdir(dir.c_str(), 0700) != 0) {
            throw std::runtime_error("failed to create subdir");
        }
        dirs_.push_back(dir);
        return dir;
    }

  private:
    std::string path_;
    std::vector<std::string> files_;
    std::vector<std::string> dirs_;
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

    StaticFileMiddleware::Options opt =
        make_options("/static", dir.path(), false);

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

TEST_F(StaticFileMiddlewareTest, ContinuesWhenPathNotInStaticPrefix) {
    TempDir dir;
    dir.write_file("app.js", "console.log('plain');");

    StaticFileMiddleware middleware(make_options("/assets", dir.path(), false));
    auto req = make_request(HttpMethod::GET, "/api/app.js");
    HttpResponse resp;

    EXPECT_TRUE(middleware.before(req, resp));
    EXPECT_EQ(resp.status_code(), HttpStatus::OK);
    EXPECT_TRUE(resp.body_content().empty());
}

TEST_F(StaticFileMiddlewareTest, RejectsNonGetAndNonHeadMethods) {
    TempDir dir;
    dir.write_file("app.js", "console.log('plain');");

    StaticFileMiddleware middleware(make_options("/assets", dir.path(), false));
    auto req = make_request(HttpMethod::POST, "/assets/app.js");
    HttpResponse resp;

    EXPECT_FALSE(middleware.before(req, resp));
    EXPECT_EQ(resp.status_code(), HttpStatus::METHOD_NOT_ALLOWED);
    EXPECT_EQ(resp.headers().at("Allow"), "GET, HEAD");
}

TEST_F(StaticFileMiddlewareTest, RejectsPathTraversal) {
    TempDir dir;
    dir.write_file("safe.txt", "safe");

    StaticFileMiddleware middleware(make_options("/assets", dir.path(), false));
    auto req = make_request(HttpMethod::GET, "/assets/../safe.txt");
    HttpResponse resp;

    EXPECT_FALSE(middleware.before(req, resp));
    EXPECT_EQ(resp.status_code(), HttpStatus::FORBIDDEN);
}

TEST_F(StaticFileMiddlewareTest, DirectoryRequestHonorsImplicitIndexSwitch) {
    TempDir dir;
    dir.make_dir("docs");
    dir.write_file("docs/index.html", "<h1>index</h1>");
    {
        StaticFileMiddleware::Options disabled =
            make_options("/assets", dir.path(), false);
        disabled.enable_implicit_index = false;
        StaticFileMiddleware middleware(disabled);
        auto req = make_request(HttpMethod::GET, "/assets/docs/");
        HttpResponse resp;
        EXPECT_FALSE(middleware.before(req, resp));
        EXPECT_EQ(resp.status_code(), HttpStatus::FORBIDDEN);
    }
    {
        StaticFileMiddleware::Options enabled =
            make_options("/assets", dir.path(), false);
        enabled.enable_implicit_index = true;
        StaticFileMiddleware middleware(enabled);
        auto req = make_request(HttpMethod::GET, "/assets/docs/");
        HttpResponse resp;
        EXPECT_FALSE(middleware.before(req, resp));
        EXPECT_EQ(resp.status_code(), HttpStatus::OK);
        EXPECT_EQ(resp.body_content(), "<h1>index</h1>");
    }
}

TEST_F(StaticFileMiddlewareTest, FallsBackToGzipThenIdentityByAcceptEncoding) {
    TempDir dir;
    dir.write_file("data.txt", "plain-data");
    dir.write_file("data.txt.gz", "gzip-data");

    StaticFileMiddleware middleware(make_options("/assets", dir.path(), false));
    {
        auto req = make_request(HttpMethod::GET, "/assets/data.txt");
        req->set_header("Accept-Encoding", "gzip");
        HttpResponse resp;
        EXPECT_FALSE(middleware.before(req, resp));
        EXPECT_EQ(resp.status_code(), HttpStatus::OK);
        EXPECT_EQ(resp.headers().at("Content-Encoding"), "gzip");
        EXPECT_EQ(resp.body_content(), "gzip-data");
    }
    {
        auto req = make_request(HttpMethod::GET, "/assets/data.txt");
        req->set_header("Accept-Encoding", "identity");
        HttpResponse resp;
        EXPECT_FALSE(middleware.before(req, resp));
        EXPECT_EQ(resp.status_code(), HttpStatus::OK);
        EXPECT_EQ(resp.headers().count("Content-Encoding"), 0U);
        EXPECT_EQ(resp.body_content(), "plain-data");
    }
}

TEST_F(StaticFileMiddlewareTest, ReturnNotModifiedWhenIfModifiedSinceMatches) {
    TempDir dir;
    dir.write_file("hello.txt", "hello");

    StaticFileMiddleware::Options opt =
        make_options("/assets", dir.path(), false);

    StaticFileMiddleware middleware(opt);

    auto first = make_request(HttpMethod::GET, "/assets/hello.txt");
    HttpResponse first_resp;
    EXPECT_FALSE(middleware.before(first, first_resp));
    ASSERT_NE(first_resp.headers().find("Last-Modified"),
              first_resp.headers().end());
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

    StaticFileMiddleware::Options opt =
        make_options("/assets", dir.path(), false);

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

TEST_F(StaticFileMiddlewareTest, ReturnNotModifiedForWildcardAndWeakEtagMatch) {
    TempDir dir;
    dir.write_file("hello.txt", "hello");

    StaticFileMiddleware middleware(make_options("/assets", dir.path(), false));

    auto first = make_request(HttpMethod::GET, "/assets/hello.txt");
    HttpResponse first_resp;
    EXPECT_FALSE(middleware.before(first, first_resp));
    ASSERT_NE(first_resp.headers().find("ETag"), first_resp.headers().end());
    const std::string etag = first_resp.headers().at("ETag");

    auto wildcard_req = make_request(HttpMethod::GET, "/assets/hello.txt");
    wildcard_req->set_header("If-None-Match", "*");
    HttpResponse wildcard_resp;
    EXPECT_FALSE(middleware.before(wildcard_req, wildcard_resp));
    EXPECT_EQ(wildcard_resp.status_code(), HttpStatus::NOT_MODIFIED);

    auto weak_req = make_request(HttpMethod::GET, "/assets/hello.txt");
    std::string weak_etag = etag;
    if (weak_etag.rfind("W/", 0) != 0 && weak_etag.rfind("w/", 0) != 0) {
        weak_etag = "W/" + weak_etag;
    }
    weak_req->set_header("If-None-Match", " " + weak_etag + " ");
    HttpResponse weak_resp;
    EXPECT_FALSE(middleware.before(weak_req, weak_resp));
    EXPECT_EQ(weak_resp.status_code(), HttpStatus::NOT_MODIFIED);
}

TEST_F(StaticFileMiddlewareTest, IfNoneMatchHasPrecedenceOverIfModifiedSince) {
    TempDir dir;
    dir.write_file("hello.txt", "hello");

    StaticFileMiddleware::Options opt =
        make_options("/assets", dir.path(), false);

    StaticFileMiddleware middleware(opt);

    auto first = make_request(HttpMethod::GET, "/assets/hello.txt");
    HttpResponse first_resp;
    EXPECT_FALSE(middleware.before(first, first_resp));
    ASSERT_NE(first_resp.headers().find("Last-Modified"),
              first_resp.headers().end());
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

    StaticFileMiddleware::Options opt =
        make_options("/assets", dir.path(), true, 10);

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

TEST_F(StaticFileMiddlewareTest, MissingFileReturnsContinueForDownstreamRoute) {
    TempDir dir;
    StaticFileMiddleware middleware(make_options("/assets", dir.path(), false));

    auto req = make_request(HttpMethod::GET, "/assets/not-exists.txt");
    HttpResponse resp;

    EXPECT_TRUE(middleware.before(req, resp));
}

TEST_F(StaticFileMiddlewareTest,
       DisablesConditionalAndVariantHeadersByOptions) {
    TempDir dir;
    dir.write_file("hello.txt", "hello");

    StaticFileMiddleware::Options opt =
        make_options("/assets", dir.path(), false);
    opt.enable_etag = false;
    opt.enable_last_modified = false;
    opt.gzip_static = false;
    opt.br_static = false;
    opt.cache_control.clear();
    StaticFileMiddleware middleware(opt);

    auto req = make_request(HttpMethod::GET, "/assets/hello.txt");
    req->set_header("If-None-Match", "*");
    HttpResponse resp;
    EXPECT_FALSE(middleware.before(req, resp));
    EXPECT_EQ(resp.status_code(), HttpStatus::OK);
    EXPECT_EQ(resp.headers().count("ETag"), 0U);
    EXPECT_EQ(resp.headers().count("Last-Modified"), 0U);
    EXPECT_EQ(resp.headers().count("Vary"), 0U);
    EXPECT_EQ(resp.headers().count("Cache-Control"), 0U);
}

TEST_F(StaticFileMiddlewareTest, ReturnNotModifiedByEtagFromMemoryCache) {
    TempDir dir;
    const std::string file_path = dir.write_file("cache.txt", "v1");

    StaticFileMiddleware::Options opt =
        make_options("/assets", dir.path(), true, 10);

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

    StaticFileMiddleware::Options opt =
        make_options("/assets", dir.path(), false);

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

    StaticFileMiddleware::Options opt =
        make_options("/assets", dir.path(), false);

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

    StaticFileMiddleware::Options opt =
        make_options("/assets", dir.path(), false);

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

    StaticFileMiddleware::Options opt =
        make_options("/assets", dir.path(), false);

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
    const std::string file_path =
        dir.write_file("cache-range.txt", "0123456789");

    StaticFileMiddleware::Options opt =
        make_options("/assets", dir.path(), true, 10);

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

TEST_F(StaticFileMiddlewareTest, SkipCachingWhenFileExceedsConfiguredMaxSize) {
    TempDir dir;
    const std::string file_path = dir.write_file("large.txt", "0123456789");

    StaticFileMiddleware::Options opt =
        make_options("/assets", dir.path(), true, 30);
    opt.max_cached_file_size = 3;
    StaticFileMiddleware middleware(opt);

    auto warm_req = make_request(HttpMethod::GET, "/assets/large.txt");
    HttpResponse warm_resp;
    EXPECT_FALSE(middleware.before(warm_req, warm_resp));
    EXPECT_EQ(warm_resp.status_code(), HttpStatus::OK);

    ::unlink(file_path.c_str());

    auto second_req = make_request(HttpMethod::GET, "/assets/large.txt");
    HttpResponse second_resp;
    EXPECT_TRUE(middleware.before(second_req, second_resp));
}

int main(int argc, char **argv) {
    zhttp::init_logger();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
