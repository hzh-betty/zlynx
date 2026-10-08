#ifndef ZHTTP_CONTENT_PARSED_REQUEST_BODY_H_
#define ZHTTP_CONTENT_PARSED_REQUEST_BODY_H_

#include <functional>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <unordered_map>

namespace zhttp {
class MultipartFormData;

using RequestParams = std::unordered_map<std::string, std::string>;
void parse_urlencoded_params(const std::string &text, RequestParams &params);

// 借用 Context 所持有的只读正文，按内容类型解析并缓存派生结果。
// multipart 解析通过回调注入，不依赖 HttpRequest。
class ParsedRequestBody {
  public:
    using Json = nlohmann::json;
    using Params = RequestParams;
    using MultipartParser =
        std::function<std::shared_ptr<MultipartFormData>(std::string *)>;

    explicit ParsedRequestBody(const std::string &body) : body_(&body) {}
    void invalidate();

    static bool is_json(const std::string &content_type);
    static bool is_form_urlencoded(const std::string &content_type);
    static bool is_multipart(const std::string &content_type);
    bool parse_json(const std::string &content_type) const;
    bool parse_form_urlencoded(const std::string &content_type) const;
    bool parse_multipart(const std::string &content_type,
                         const MultipartParser &parser) const;
    const Json *json(const std::string &content_type) const;
    const Params &form_params(const std::string &content_type) const;
    const MultipartFormData *multipart(const std::string &content_type,
                                       const MultipartParser &parser) const;
    const std::string &json_error() const { return json_error_; }
    const std::string &multipart_error() const { return multipart_error_; }

  private:
    const std::string *body_;
    mutable bool multipart_parsed_ = false;
    mutable std::shared_ptr<MultipartFormData> multipart_;
    mutable std::string multipart_error_;
    mutable bool json_parsed_ = false;
    mutable std::shared_ptr<Json> json_;
    mutable std::string json_error_;
    mutable bool form_parsed_ = false;
    mutable Params form_params_;
};

} // namespace zhttp

#endif
