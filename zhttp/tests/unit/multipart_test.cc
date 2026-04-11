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

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
