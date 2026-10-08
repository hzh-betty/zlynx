#include "zhttp/content/parsed_request_body.h"

#include "zhttp/http_common.h"
#include <nlohmann/json.hpp>
#include <utility>

namespace zhttp {

void parse_urlencoded_params(const std::string &text, RequestParams &params) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('&', pos);
        if (end == std::string::npos) {
            end = text.size();
        }
        const std::string pair = text.substr(pos, end - pos);
        const size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            params[url_decode(pair.substr(0, eq))] =
                url_decode(pair.substr(eq + 1));
        } else if (!pair.empty()) {
            params[url_decode(pair)] = "";
        }
        pos = end + 1;
    }
}

bool ParsedRequestBody::is_json(const std::string &content_type) {
    return normalize_mime_type(content_type) == "application/json";
}

bool ParsedRequestBody::is_form_urlencoded(const std::string &content_type) {
    return normalize_mime_type(content_type) ==
           "application/x-www-form-urlencoded";
}

bool ParsedRequestBody::is_multipart(const std::string &content_type) {
    return normalize_mime_type(content_type) == "multipart/form-data";
}

bool ParsedRequestBody::parse_json(const std::string &content_type) const {
    // 已经解析过则直接复用缓存结果。
    if (json_parsed_) {
        return json_ != nullptr;
    }

    // 标记已解析并清理旧状态，开始新一轮解析。
    json_parsed_ = true;
    json_.reset();
    json_error_.clear();

    // 不是 JSON 请求：按“无需解析”处理。
    if (!is_json(content_type)) {
        return true;
    }

    // JSON 请求但 body 为空，显式给出错误。
    if (body_->empty()) {
        json_error_ = "Empty JSON body";
        return false;
    }

    // 使用 nlohmann::json 非异常模式解析，失败时通过 is_discarded() 判断。
    Json parsed = Json::parse(*body_, nullptr, false);
    if (parsed.is_discarded()) {
        json_error_ = "Invalid JSON body";
        return false;
    }

    json_ = std::make_shared<Json>(std::move(parsed));
    return true;
}

bool ParsedRequestBody::parse_form_urlencoded(
    const std::string &content_type) const {
    // 表单解析结果无失败分支，解析过后直接复用。
    if (form_parsed_) {
        return true;
    }

    form_parsed_ = true;
    form_params_.clear();

    if (!is_form_urlencoded(content_type)) {
        return true;
    }

    parse_urlencoded_params(*body_, form_params_);
    return true;
}

bool ParsedRequestBody::parse_multipart(const std::string &content_type,
                                        const MultipartParser &parser) const {
    if (multipart_parsed_) {
        return multipart_ != nullptr;
    }

    // 先清理旧状态，再开始新一轮解析。
    multipart_parsed_ = true;
    multipart_.reset();
    multipart_error_.clear();

    if (!is_multipart(content_type)) {
        return true;
    }

    auto parsed = parser(&multipart_error_);
    if (!parsed) {
        return false;
    }
    multipart_ = std::move(parsed);
    return true;
}

const ParsedRequestBody::Json *
ParsedRequestBody::json(const std::string &content_type) const {
    if (!json_parsed_) {
        parse_json(content_type);
    }
    return json_.get();
}

const ParsedRequestBody::Params &
ParsedRequestBody::form_params(const std::string &content_type) const {
    if (!form_parsed_) {
        parse_form_urlencoded(content_type);
    }
    return form_params_;
}

const MultipartFormData *
ParsedRequestBody::multipart(const std::string &content_type,
                             const MultipartParser &parser) const {
    if (!multipart_parsed_) {
        parse_multipart(content_type, parser);
    }
    return multipart_.get();
}

void ParsedRequestBody::invalidate() {
    multipart_parsed_ = false;
    multipart_.reset();
    multipart_error_.clear();

    json_parsed_ = false;
    json_.reset();
    json_error_.clear();

    form_parsed_ = false;
    form_params_.clear();
}

} // namespace zhttp
