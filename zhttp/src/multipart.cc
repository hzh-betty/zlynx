#include "zhttp/multipart.h"

#include "zhttp/http_request.h"
#include "zhttp/http_utils.h"
#include "zhttp/http_common.h"

#include <sstream>

namespace zhttp {

namespace {
static bool extract_boundary(const std::string &content_type,
                             std::string &boundary) {
    // 从 Content-Type 参数中提取 boundary，兼容 boundary="xxx" 这种带引号写法。
    // 典型头部：multipart/form-data; boundary=----WebKitFormBoundaryabc
    // 这里不假设参数顺序，只要在 ';' 之后的参数区里找到 boundary= 即可。
    std::string ct = content_type;
    auto semi = ct.find(';');
    if (semi == std::string::npos) {
        // 只有主类型、没有任何参数，按无 boundary 处理。
        return false;
    }

    // 按 ';' 拆分参数列表，每个 token 形如 key=value。
    std::string params = ct.substr(semi + 1);
    std::istringstream iss(params);
    std::string token;
    while (std::getline(iss, token, ';')) {
        trim(token);
        if (token.size() >= 9 && to_lower(token.substr(0, 9)) == "boundary=") {
            boundary = token.substr(9);
            trim(boundary);
            // RFC 中参数值允许用双引号包裹，去掉外层引号后再使用。
            if (!boundary.empty() && boundary.front() == '"' &&
                boundary.back() == '"' && boundary.size() >= 2) {
                boundary = boundary.substr(1, boundary.size() - 2);
            }
            // 空 boundary 视为非法（后续无法正确定位分隔符）。
            return !boundary.empty();
        }
    }
    // 遍历完参数仍未找到 boundary=。
    return false;
}

static bool parse_content_disposition(const std::string &value,
                                      std::string &name,
                                      std::string &filename) {
    // 解析形如 form-data; name="field"; filename="a.txt" 的参数串。
    // 我们只依赖 name/filename 两个参数，其他扩展参数（如 filename*）当前忽略。
    name.clear();
    filename.clear();

    // Content-Disposition 同样是 ';' 分隔的参数列表。
    std::istringstream iss(value);
    std::string token;
    bool first = true;
    while (std::getline(iss, token, ';')) {
        trim(token);
        if (token.empty()) {
            continue;
        }
        if (first) {
            // 第一个 token 通常是 disposition-type（如 form-data），
            // 与字段名/文件名无关，这里直接跳过。
            first = false;
            continue;
        }

        auto eq = token.find('=');
        if (eq == std::string::npos) {
            continue;
        }

        std::string k = token.substr(0, eq);
        std::string v = token.substr(eq + 1);
        trim(k);
        trim(v);
        // 参数值若被双引号包裹，去掉外层引号。
        if (!v.empty() && v.front() == '"' && v.back() == '"' &&
            v.size() >= 2) {
            v = v.substr(1, v.size() - 2);
        }

        // 参数名按大小写不敏感处理。
        if (to_lower(k) == "name") {
            name = v;
        } else if (to_lower(k) == "filename") {
            filename = v;
        }
    }

    // multipart/form-data 中 name 是必需字段；没有 name 则该 part 不可路由。
    return !name.empty();
}

static bool
parse_part_headers(const std::string &headers_blob,
                   std::unordered_map<std::string, std::string> &headers_out) {
    // multipart 的每个 part 也有自己的一组头部，格式和普通 HTTP 头部类似。
    // 输入是“已剥离头体分隔空行”的纯头部区域（多行 CRLF 拼接）。
    headers_out.clear();
    size_t pos = 0;
    while (pos < headers_blob.size()) {
        // 逐行读取，兼容最后一行没有 CRLF 结尾的情况。
        size_t end = headers_blob.find("\r\n", pos);
        if (end == std::string::npos) {
            end = headers_blob.size();
        }
        std::string line = headers_blob.substr(pos, end - pos);
        if (!line.empty()) {
            // 头部格式：Key: Value。只按第一个 ':' 切分。
            auto colon = line.find(':');
            if (colon != std::string::npos) {
                std::string k = line.substr(0, colon);
                std::string v = line.substr(colon + 1);
                trim(k);
                trim(v);
                if (!k.empty()) {
                    // 统一小写键名，便于后续无大小写差别地查找。
                    headers_out[to_lower(k)] = v;
                }
            }
        }

        if (end == headers_blob.size()) {
            break;
        }
        pos = end + 2;
    }
    return true;
}

} // namespace

bool UploadedFile::save_to(const std::string &filepath,
                           std::string *error) const {
    if (!FileOperator::write_file_binary(filepath, data)) {
        if (error) {
            *error = "Failed to write file: " + filepath;
        }
        return false;
    }
    return true;
}

std::string MultipartFormData::field(const std::string &key,
                                     const std::string &default_val) const {
    // 表单字段按名字直接查找；未命中时返回调用方提供的默认值。
    auto it = fields_.find(key);
    if (it != fields_.end()) {
        return it->second;
    }
    return default_val;
}

const UploadedFile *
MultipartFormData::file(const std::string &field_name) const {
    // 当前接口返回同名字段的第一个文件。
    for (const auto &f : files_) {
        if (f.field_name == field_name) {
            return &f;
        }
    }
    return nullptr;
}

MultipartFormData::ptr MultipartFormData::parse(const HttpRequest &request,
                                                std::string *error) {
    if (error) {
        error->clear();
    }

    // 非 multipart 请求按“空结果”处理，调用方无需把它视为异常。
    std::string ct = request.content_type();
    if (to_lower(ct).find("multipart/form-data") == std::string::npos) {
        // 与 URL-encoded/JSON 等请求体类型共存时，调用方可统一调用 parse，
        // 再通过返回对象是否有字段/文件判断结果。
        return std::make_shared<MultipartFormData>();
    }

    std::string boundary;
    if (!extract_boundary(ct, boundary)) {
        if (error) {
            *error = "Missing boundary in Content-Type";
        }
        return nullptr;
    }

    const std::string &body = request.body();
    const std::string dash_boundary = "--" + boundary;
    // 注意 body 中实际分隔符是 "--" + boundary（不是裸 boundary）。

    // multipart Body 由多个 --boundary 包围的 part 组成，先定位起始 boundary。
    size_t pos = body.find(dash_boundary);
    if (pos == std::string::npos) {
        if (error) {
            *error = "Boundary not found in body";
        }
        return nullptr;
    }

    auto out = std::make_shared<MultipartFormData>();

    while (true) {
        // 每轮循环处理一个 part，当前位置应当正好落在 --boundary 上。
        if (body.compare(pos, dash_boundary.size(), dash_boundary) != 0) {
            // 当前位置不再是 boundary，说明结构已结束或输入已损坏。
            break;
        }
        pos += dash_boundary.size();

        // 遇到 --boundary-- 说明所有 part 都解析完了。
        if (pos + 2 <= body.size() && body.compare(pos, 2, "--") == 0) {
            // 终止 boundary 不再携带内容，直接结束解析。
            break;
        }

        // 标准写法里 boundary 后面紧跟 CRLF，再开始 part 头部。
        if (pos + 2 <= body.size() && body.compare(pos, 2, "\r\n") == 0) {
            pos += 2;
        } else {
            // 为了兼容非标准输入，这里宽容接受单个换行符。
            if (pos + 1 <= body.size() && body[pos] == '\n') {
                pos += 1;
            }
        }

        // part 头部和正文之间同样用空行分隔。
        size_t headers_end = body.find("\r\n\r\n", pos);
        if (headers_end == std::string::npos) {
            if (error) {
                *error = "Invalid multipart: missing header terminator";
            }
            return nullptr;
        }

        std::string headers_blob = body.substr(pos, headers_end - pos);
        pos = headers_end + 4;

        std::unordered_map<std::string, std::string> part_headers;
        parse_part_headers(headers_blob, part_headers);

        // 每个 form-data part 至少需要 Content-Disposition（用于拿到 name）。
        auto it_cd = part_headers.find("content-disposition");
        if (it_cd == part_headers.end()) {
            if (error) {
                *error = "Invalid multipart: missing Content-Disposition";
            }
            return nullptr;
        }

        std::string name;
        std::string filename;
        if (!parse_content_disposition(it_cd->second, name, filename)) {
            if (error) {
                *error = "Invalid multipart: Content-Disposition has no name";
            }
            return nullptr;
        }

        std::string part_ct;
        auto it_ct = part_headers.find("content-type");
        if (it_ct != part_headers.end()) {
            // 文件 part 通常会携带具体 MIME，普通字段可为空。
            part_ct = it_ct->second;
        }

        // 当前 part 的正文以“CRLF + 下一个 boundary”为结束标记。
        std::string marker = "\r\n" + dash_boundary;
        size_t next = body.find(marker, pos);
        if (next == std::string::npos) {
            // 即使是最后一个 part，后面也应当还能看到结束 boundary。
            if (error) {
                *error = "Invalid multipart: missing next boundary";
            }
            return nullptr;
        }

        std::string part_data = body.substr(pos, next - pos);
        pos = next + 2; // 跳过 marker 前导 CRLF，令 pos 重新指向 --boundary

        if (!filename.empty()) {
            // 只要带 filename，就按文件上传处理。
            UploadedFile f;
            f.field_name = name;
            f.filename = filename;
            f.content_type = part_ct;
            f.data = std::move(part_data);
            out->files_.push_back(std::move(f));
        } else {
            // 否则当作普通文本字段处理。
            // 这里采用“同名字段后写覆盖前写”的语义；若要保留多值可改为 vector。
            out->fields_[name] = std::move(part_data);
        }

        // 跳到下一个 boundary，继续处理下一个 part。
        size_t bpos = body.find(dash_boundary, pos);
        if (bpos == std::string::npos) {
            break;
        }
        pos = bpos;
    }

    return out;
}

} // namespace zhttp
