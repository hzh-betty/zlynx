#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

#include <fstream>

#include "zhttp/http_request.h"
#include "zhttp/multipart.h"

using namespace zhttp;

class MultipartTest : public ::testing::Test {
  protected:
    HttpRequest::ptr make_request(const std::string &boundary,
                                  const std::string &body) {
        auto req = std::make_shared<HttpRequest>();
        req->set_method(HttpMethod::POST);
        req->set_path("/upload");
        req->set_header("Content-Type",
                        "multipart/form-data; boundary=" + boundary);
        req->set_body(body);
        return req;
    }
};

TEST_F(MultipartTest, ParseFieldsAndFile) {
    std::string body;
    body += "--abc\r\n";
    body += "Content-Disposition: form-data; name=\"text\"\r\n";
    body += "\r\n";
    body += "hello\r\n";
    body += "--abc\r\n";
    body +=
        "Content-Disposition: form-data; name=\"file\"; filename=\"a.txt\"\r\n";
    body += "Content-Type: text/plain\r\n";
    body += "\r\n";
    body += "filecontent\r\n";
    body += "--abc--\r\n";

    auto req = make_request("abc", body);

    ASSERT_TRUE(req->parse_multipart()) << req->multipart_error();
    const MultipartFormData *mp = req->multipart();
    ASSERT_NE(mp, nullptr);

    EXPECT_EQ(mp->field("text"), "hello");

    const UploadedFile *f = mp->file("file");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->filename, "a.txt");
    EXPECT_EQ(f->content_type, "text/plain");
    EXPECT_EQ(f->data, "filecontent");
}

TEST_F(MultipartTest, MissingBoundary_Fails) {
    auto req = std::make_shared<HttpRequest>();
    req->set_method(HttpMethod::POST);
    req->set_path("/upload");
    req->set_header("Content-Type", "multipart/form-data");
    req->set_body("--abc--\r\n");

    EXPECT_FALSE(req->parse_multipart());
    EXPECT_FALSE(req->multipart_error().empty());
}

TEST_F(MultipartTest, ParseMultipleFiles) {
    std::string body;
    body += "--b\r\n";
    body += "Content-Disposition: form-data; name=\"file1\"; "
            "filename=\"1.bin\"\r\n";
    body += "\r\n";
    body += "111\r\n";
    body += "--b\r\n";
    body += "Content-Disposition: form-data; name=\"file2\"; "
            "filename=\"2.bin\"\r\n";
    body += "Content-Type: application/octet-stream\r\n";
    body += "\r\n";
    body += "222\r\n";
    body += "--b--\r\n";

    auto req = make_request("b", body);
    ASSERT_TRUE(req->parse_multipart()) << req->multipart_error();
    const MultipartFormData *mp = req->multipart();
    ASSERT_NE(mp, nullptr);

    const UploadedFile *f1 = mp->file("file1");
    const UploadedFile *f2 = mp->file("file2");
    ASSERT_NE(f1, nullptr);
    ASSERT_NE(f2, nullptr);
    EXPECT_EQ(f1->filename, "1.bin");
    EXPECT_EQ(f1->data, "111");
    EXPECT_EQ(f2->filename, "2.bin");
    EXPECT_EQ(f2->content_type, "application/octet-stream");
    EXPECT_EQ(f2->data, "222");
}

TEST_F(MultipartTest, UploadedFileSaveTo_WritesFile) {
    std::string body;
    body += "--b\r\n";
    body +=
        "Content-Disposition: form-data; name=\"file\"; filename=\"a.txt\"\r\n";
    body += "Content-Type: text/plain\r\n";
    body += "\r\n";
    body += "hello-file\r\n";
    body += "--b--\r\n";

    auto req = make_request("b", body);
    ASSERT_TRUE(req->parse_multipart()) << req->multipart_error();
    const MultipartFormData *mp = req->multipart();
    ASSERT_NE(mp, nullptr);

    const UploadedFile *f = mp->file("file");
    ASSERT_NE(f, nullptr);

    char path[] = "/tmp/zhttp_upload_test_XXXXXX";
    int fd = ::mkstemp(path);
    ASSERT_GE(fd, 0);
    ::close(fd);

    std::string err;
    ASSERT_TRUE(f->save_to(path, &err)) << err;

    std::ifstream in(path, std::ios::in | std::ios::binary);
    ASSERT_TRUE(in.good());
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "hello-file");

    ::unlink(path);
}

TEST_F(MultipartTest, NonMultipartContentTypeReturnsEmptyResult) {
    auto req = std::make_shared<HttpRequest>();
    req->set_method(HttpMethod::POST);
    req->set_path("/submit");
    req->set_header("Content-Type", "application/json");
    req->set_body("{\"k\":\"v\"}");

    ASSERT_TRUE(req->parse_multipart());
    EXPECT_EQ(req->multipart(), nullptr);
}

TEST_F(MultipartTest, SupportsQuotedBoundaryAndLowercaseHeaderNames) {
    auto req = std::make_shared<HttpRequest>();
    req->set_method(HttpMethod::POST);
    req->set_path("/upload");
    req->set_header(
        "Content-Type",
        "multipart/form-data; charset=utf-8; boundary=\"quoted-b\"");

    std::string body;
    body += "--quoted-b\r\n";
    body += "content-disposition: form-data; name=\"text\"\r\n";
    body += "\r\n";
    body += "hello-quoted\r\n";
    body += "--quoted-b--\r\n";
    req->set_body(body);

    ASSERT_TRUE(req->parse_multipart()) << req->multipart_error();
    const MultipartFormData *mp = req->multipart();
    ASSERT_NE(mp, nullptr);
    EXPECT_EQ(mp->field("text"), "hello-quoted");
}

TEST_F(MultipartTest, InvalidMultipartStructuresReturnErrors) {
    {
        // body 中找不到 boundary。
        auto req = make_request("abc", "no-boundary-here");
        EXPECT_FALSE(req->parse_multipart());
        EXPECT_NE(req->multipart_error().find("Boundary"), std::string::npos);
    }

    {
        // 缺失 header/body 分隔空行。
        std::string body;
        body += "--abc\r\n";
        body += "Content-Disposition: form-data; name=\"x\"\r\n";
        body += "--abc--\r\n";
        auto req = make_request("abc", body);
        EXPECT_FALSE(req->parse_multipart());
        EXPECT_NE(req->multipart_error().find("header terminator"),
                  std::string::npos);
    }

    {
        // 缺失 Content-Disposition。
        std::string body;
        body += "--abc\r\n";
        body += "Content-Type: text/plain\r\n";
        body += "\r\n";
        body += "abc\r\n";
        body += "--abc--\r\n";
        auto req = make_request("abc", body);
        EXPECT_FALSE(req->parse_multipart());
        EXPECT_NE(req->multipart_error().find("Content-Disposition"),
                  std::string::npos);
    }

    {
        // Content-Disposition 无 name 参数。
        std::string body;
        body += "--abc\r\n";
        body += "Content-Disposition: form-data; filename=\"a.txt\"\r\n";
        body += "\r\n";
        body += "data\r\n";
        body += "--abc--\r\n";
        auto req = make_request("abc", body);
        EXPECT_FALSE(req->parse_multipart());
        EXPECT_NE(req->multipart_error().find("no name"), std::string::npos);
    }

    {
        // 找不到下一个 boundary。
        std::string body;
        body += "--abc\r\n";
        body += "Content-Disposition: form-data; name=\"x\"\r\n";
        body += "\r\n";
        body += "value";
        auto req = make_request("abc", body);
        EXPECT_FALSE(req->parse_multipart());
        EXPECT_NE(req->multipart_error().find("next boundary"),
                  std::string::npos);
    }
}

TEST_F(MultipartTest, DuplicateFieldUsesLatestValueAndFileLookupMisses) {
    std::string body;
    body += "--b\r\n";
    body += "Content-Disposition: form-data; name=\"k\"\r\n";
    body += "\r\n";
    body += "v1\r\n";
    body += "--b\r\n";
    body += "Content-Disposition: form-data; name=\"k\"\r\n";
    body += "\r\n";
    body += "v2\r\n";
    body += "--b--\r\n";

    auto req = make_request("b", body);
    ASSERT_TRUE(req->parse_multipart()) << req->multipart_error();
    const MultipartFormData *mp = req->multipart();
    ASSERT_NE(mp, nullptr);
    EXPECT_EQ(mp->field("k"), "v2");
    EXPECT_EQ(mp->file("not-exist"), nullptr);
}

TEST_F(MultipartTest, UploadedFileSaveToFailureReturnsErrorText) {
    std::string body;
    body += "--b\r\n";
    body +=
        "Content-Disposition: form-data; name=\"file\"; filename=\"a.txt\"\r\n";
    body += "Content-Type: text/plain\r\n";
    body += "\r\n";
    body += "hello-file\r\n";
    body += "--b--\r\n";

    auto req = make_request("b", body);
    ASSERT_TRUE(req->parse_multipart()) << req->multipart_error();
    const UploadedFile *f = req->multipart()->file("file");
    ASSERT_NE(f, nullptr);

    std::string err;
    EXPECT_FALSE(f->save_to("/tmp", &err));
    EXPECT_NE(err.find("Failed to write file"), std::string::npos);
}

TEST_F(MultipartTest,
       SupportsBoundaryWithUppercaseTokenAndRejectsEmptyBoundary) {
    {
        auto req = std::make_shared<HttpRequest>();
        req->set_method(HttpMethod::POST);
        req->set_path("/upload");
        req->set_header("Content-Type",
                        "multipart/form-data; charset=utf-8; BOUNDARY=AbC");

        std::string body;
        body += "--AbC\r\n";
        body += "Content-Disposition: form-data; flag; name=\"k\"\r\n";
        body += ": ignored-empty-key\r\n";
        body += "NoColonHeader\r\n";
        body += "\r\n";
        body += "v\r\n";
        body += "--AbC--\r\n";
        req->set_body(body);

        ASSERT_TRUE(req->parse_multipart()) << req->multipart_error();
        const MultipartFormData *mp = req->multipart();
        ASSERT_NE(mp, nullptr);
        EXPECT_EQ(mp->field("k"), "v");
        EXPECT_EQ(mp->field("missing", "default-v"), "default-v");
    }

    {
        auto req = std::make_shared<HttpRequest>();
        req->set_method(HttpMethod::POST);
        req->set_path("/upload");
        req->set_header("Content-Type", "multipart/form-data; boundary=\"\"");
        req->set_body("--x--\r\n");
        EXPECT_FALSE(req->parse_multipart());
        EXPECT_NE(req->multipart_error().find("Missing boundary"),
                  std::string::npos);
    }
}

TEST_F(MultipartTest, AcceptsSingleLfAfterBoundaryLine) {
    auto req = std::make_shared<HttpRequest>();
    req->set_method(HttpMethod::POST);
    req->set_path("/upload");
    req->set_header("Content-Type",
                    "multipart/form-data; boundary=lf-boundary");

    std::string body;
    body += "--lf-boundary\n";
    body += "Content-Disposition: form-data; name=\"k\"\r\n";
    body += "\r\n";
    body += "lf-ok\r\n";
    body += "--lf-boundary--\r\n";
    req->set_body(body);

    ASSERT_TRUE(req->parse_multipart()) << req->multipart_error();
    const MultipartFormData *mp = req->multipart();
    ASSERT_NE(mp, nullptr);
    EXPECT_EQ(mp->field("k"), "lf-ok");
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
