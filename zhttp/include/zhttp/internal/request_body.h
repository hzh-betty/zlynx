#ifndef ZHTTP_INTERNAL_REQUEST_BODY_H_
#define ZHTTP_INTERNAL_REQUEST_BODY_H_

#include <functional>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <unordered_map>

namespace zhttp {
class MultipartFormData;
namespace detail {

using RequestParams = std::unordered_map<std::string, std::string>;
void parse_urlencoded_params(const std::string &text, RequestParams &params);

// 原始请求体及其派生缓存共用同一失效边界。
// multipart 解析通过回调注入，不依赖 HttpRequest。
class RequestBody {
  public:
    using Json = nlohmann::json;
    using Params = RequestParams;
    using MultipartParser =
        std::function<std::shared_ptr<MultipartFormData>(std::string *)>;

    const std::string &content() const { return body_; }
    void set(const std::string &body);
    void set(std::string &&body);
    void invalidate();

    static bool is_json(const std::string &content_type);
    static bool is_form_urlencoded(const std::string &content_type);
    static bool is_multipart(const std::string &content_type);
    bool parse_json(const std::string &content_type);
    bool parse_form_urlencoded(const std::string &content_type);
    bool parse_multipart(const std::string &content_type,
                         const MultipartParser &parser);
    const Json *json(const std::string &content_type) const;
    const Params &form_params(const std::string &content_type) const;
    const MultipartFormData *multipart(const std::string &content_type,
                                       const MultipartParser &parser) const;
    const std::string &json_error() const { return json_error_; }
    const std::string &multipart_error() const { return multipart_error_; }

  private:
    std::string body_;
    bool multipart_parsed_ = false;
    std::shared_ptr<MultipartFormData> multipart_;
    std::string multipart_error_;
    bool json_parsed_ = false;
    std::shared_ptr<Json> json_;
    std::string json_error_;
    bool form_parsed_ = false;
    Params form_params_;
};
} // 命名空间 detail
} // 命名空间 zhttp

#endif
