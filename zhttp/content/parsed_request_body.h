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
/** 解析 URL 编码键值并写入 params；保留现有其他键，重复键取最后值。 */
void parse_urlencoded_params(const std::string &text, RequestParams &params);

// 借用 Context 所持有的只读正文，按内容类型解析并缓存派生结果。
// multipart 解析通过回调注入，不依赖 HttpRequest。
/**
 * 借用正文的惰性解析缓存。
 *
 * body 必须存活至本对象析构；正文改变后先调用 invalidate()。缓存不支持并发访问。
 */
class ParsedRequestBody {
  public:
    using Json = nlohmann::json;
    using Params = RequestParams;
    using MultipartParser =
        std::function<std::shared_ptr<MultipartFormData>(std::string *)>;

    /** 借用正文引用，不复制字节。 */
    explicit ParsedRequestBody(const std::string &body) : body_(&body) {}
    /** 清除所有派生缓存、解析标记和错误。 */
    void invalidate();

    /** 归一化 MIME 类型并判断是否为 application/json。 */
    static bool is_json(const std::string &content_type);
    /** 判断是否为 application/x-www-form-urlencoded。 */
    static bool is_form_urlencoded(const std::string &content_type);
    /** 判断是否为 multipart/form-data。 */
    static bool is_multipart(const std::string &content_type);
    /** 首次解析并缓存 JSON；首次类型不符视为无需解析，JSON 解析失败返回 false。 */
    bool parse_json(const std::string &content_type) const;
    /** 首次解析并缓存表单；类型不符时保留空缓存并返回 true。 */
    bool parse_form_urlencoded(const std::string &content_type) const;
    /** 首次通过 parser 解析 multipart；首次类型不符视为无需解析，解析失败返回 false。 */
    bool parse_multipart(const std::string &content_type,
                         const MultipartParser &parser) const;
    /** 返回惰性 JSON 缓存，类型不符或解析失败返回 nullptr。 */
    const Json *json(const std::string &content_type) const;
    /** 返回表单缓存，类型不符时为空。 */
    const Params &form_params(const std::string &content_type) const;
    /** 返回惰性 multipart 缓存，类型不符或解析失败返回 nullptr。 */
    const MultipartFormData *multipart(const std::string &content_type,
                                       const MultipartParser &parser) const;
    /** 返回 JSON 解析错误，未发生错误时为空。 */
    const std::string &json_error() const { return json_error_; }
    /** 返回 multipart 解析错误，未发生错误时为空。 */
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
