#include "zhttp/detail/parsed_request_body.h"

#include "zhttp/http_common.h"
#include <nlohmann/json.hpp>
#include <utility>

namespace zhttp {
/*
 * application/x-www-form-urlencoded 请求体格式
 *
 * Content-Type: application/x-www-form-urlencoded
 *
 * 请求体示例：
 *
 * username=zhangsan&age=20&city=Chengdu&message=hello+world
 *
 * 格式说明：
 *
 * 1. 键值对（Key-Value）
 *    - 每个表单字段由 key=value 组成。
 *    - key 和 value 均需要按照表单 URL 编码规则进行编码。
 *
 * 2. 字段分隔符（&）
 *    - 使用 '&' 分隔多个键值对。
 *    - 例如：name=Tom&age=20
 *
 * 3. 键值分隔符（=）
 *    - 使用 '=' 分隔字段名称与字段值。
 *    - 第一个 '=' 通常作为键值分隔符。
 *    - 值本身包含 '=' 时，应编码为 %3D。
 *
 * 4. 空格编码
 *    - 表单 URL 编码中，空格通常编码为 '+'。
 *    - 例如：hello world -> hello+world
 *
 * 5. 百分号编码（Percent-Encoding）
 *    - 特殊字符及非 ASCII 字节可使用 %HH 编码。
 *    - HH 表示一个十六进制字节。
 *    - 例如：
 *        '&'  -> %26
 *        '='  -> %3D
 *        '+'  -> %2B
 *        '%'  -> %25
 *        中文字符需要先转换为 UTF-8 字节，再逐字节进行百分号编码。
 *
 * 6. 空值与空字段
 *    - name= 表示字段 name 的值为空。
 *    - name 也可以表示没有显式 '=' 的字段，但解析器应明确约定其处理方式。
 *    - name=Tom&name=Jerry 允许出现重复字段名，不能默认后者覆盖前者。
 *
 * 7. 解码规则
 *    - '+' 解码为空格。
 *    - %HH 解码为对应的一个字节。
 *    - 解码时应检查 '%' 后是否存在两个合法的十六进制字符。
 *    - 如果字段内容要求 UTF-8，应在百分号解码后验证 UTF-8 编码。
 *
 * 注意：
 *    - 该格式通常用于 HTML 表单提交，不适合直接承载原始二进制文件。
 *    - 请求体的长度由 HTTP 消息定界机制确定，例如 Content-Length
 *      或适用协议版本中的传输分帧机制。
 *    - '+' 在该格式中代表空格；字面量 '+' 必须编码为 %2B。
 *    - 该格式属于表单编码，不是 HTTP Transfer-Encoding。
 */


void parse_urlencoded_params(std::string_view text, RequestParams &params) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('&', pos);
        if (end == std::string::npos) {
            end = text.size();
        }
        const auto pair = text.substr(pos, end - pos);
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

namespace detail {
ParsedRequestBody::ParsedRequestBody(const std::string &body) : body_(&body) {}

ParsedRequestBody::~ParsedRequestBody() = default;

bool ParsedRequestBody::is_json(std::string_view content_type) {
    return normalize_mime_type(content_type) == "application/json";
}

bool ParsedRequestBody::is_form_urlencoded(std::string_view content_type) {
    return normalize_mime_type(content_type) ==
           "application/x-www-form-urlencoded";
}

bool ParsedRequestBody::is_multipart(std::string_view content_type) {
    return normalize_mime_type(content_type) == "multipart/form-data";
}

bool ParsedRequestBody::parse_json(std::string_view content_type) const {
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

    json_ = std::make_unique<Json>(std::move(parsed));
    return true;
}

bool ParsedRequestBody::parse_form_urlencoded(
    std::string_view content_type) const {
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

bool ParsedRequestBody::parse_multipart(std::string_view content_type,
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
ParsedRequestBody::json(std::string_view content_type) const {
    if (!json_parsed_) {
        parse_json(content_type);
    }
    return json_.get();
}

const ParsedRequestBody::Params &
ParsedRequestBody::form_params(std::string_view content_type) const {
    if (!form_parsed_) {
        parse_form_urlencoded(content_type);
    }
    return form_params_;
}

const MultipartFormData *
ParsedRequestBody::multipart(std::string_view content_type,
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

} // namespace detail
} // namespace zhttp
