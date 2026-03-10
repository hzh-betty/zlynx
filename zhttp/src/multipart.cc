#include "multipart.h"

#include "http_request.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace zhttp {

namespace {

static inline std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

// 就地裁剪首尾空白，便于解析头字段参数。
static inline void trim(std::string &s) {
  auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
  while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) {
    s.erase(s.begin());
  }
  while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) {
    s.pop_back();
  }
}

static bool extract_boundary(const std::string &content_type,
                             std::string &boundary) {
  // 从 Content-Type 参数中提取 boundary，兼容带引号的写法。
  std::string ct = content_type;
  auto semi = ct.find(';');
  if (semi == std::string::npos) {
    return false;
  }

  std::string params = ct.substr(semi + 1);
  std::istringstream iss(params);
  std::string token;
  while (std::getline(iss, token, ';')) {
    trim(token);
    if (token.size() >= 9 && to_lower(token.substr(0, 9)) == "boundary=") {
      boundary = token.substr(9);
      trim(boundary);
      if (!boundary.empty() && boundary.front() == '"' &&
          boundary.back() == '"' && boundary.size() >= 2) {
        boundary = boundary.substr(1, boundary.size() - 2);
      }
      return !boundary.empty();
    }
  }
  return false;
}

static bool parse_content_disposition(const std::string &value,
                                      std::string &name,
                                      std::string &filename) {
  // 解析形如 form-data; name="field"; filename="a.txt" 的参数串。
  name.clear();
  filename.clear();

  std::istringstream iss(value);
  std::string token;
  bool first = true;
  while (std::getline(iss, token, ';')) {
    trim(token);
    if (token.empty()) {
      continue;
    }
    if (first) {
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
    if (!v.empty() && v.front() == '"' && v.back() == '"' && v.size() >= 2) {
      v = v.substr(1, v.size() - 2);
    }

    if (to_lower(k) == "name") {
      name = v;
    } else if (to_lower(k) == "filename") {
      filename = v;
    }
  }

  return !name.empty();
}

static bool parse_part_headers(const std::string &headers_blob,
                               std::unordered_map<std::string, std::string>
                                   &headers_out) {
  // multipart 每个 part 的头部仍是标准 HTTP 风格的 Key: Value 列表。
  headers_out.clear();
  size_t pos = 0;
  while (pos < headers_blob.size()) {
    size_t end = headers_blob.find("\r\n", pos);
    if (end == std::string::npos) {
      end = headers_blob.size();
    }
    std::string line = headers_blob.substr(pos, end - pos);
    if (!line.empty()) {
      auto colon = line.find(':');
      if (colon != std::string::npos) {
        std::string k = line.substr(0, colon);
        std::string v = line.substr(colon + 1);
        trim(k);
        trim(v);
        if (!k.empty()) {
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
  // 直接按二进制落盘，避免文本模式破坏上传内容。
  std::ofstream ofs(filepath.c_str(), std::ios::binary);
  if (!ofs) {
    if (error) {
      *error = "Failed to open file for write: " + filepath;
    }
    return false;
  }
  ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
  if (!ofs) {
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

const UploadedFile *MultipartFormData::file(const std::string &field_name) const {
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

  // 非 multipart 请求按“空结果”处理，调用方不需要区分异常和非该类型请求。
  std::string ct = request.content_type();
  if (to_lower(ct).find("multipart/form-data") == std::string::npos) {
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

  // 找到第一个 boundary
  size_t pos = body.find(dash_boundary);
  if (pos == std::string::npos) {
    if (error) {
      *error = "Boundary not found in body";
    }
    return nullptr;
  }

  auto out = std::make_shared<MultipartFormData>();

  while (true) {
    // 每轮循环处理一个 part，当前位置应当落在 --boundary 上。
    if (body.compare(pos, dash_boundary.size(), dash_boundary) != 0) {
      break;
    }
    pos += dash_boundary.size();

    // 结束标记 --boundary--
    if (pos + 2 <= body.size() && body.compare(pos, 2, "--") == 0) {
      break;
    }

    // boundary 后应接 CRLF
    if (pos + 2 <= body.size() && body.compare(pos, 2, "\r\n") == 0) {
      pos += 2;
    } else {
      // 宽容：允许只有 \n
      if (pos + 1 <= body.size() && body[pos] == '\n') {
        pos += 1;
      }
    }

    // 解析 part headers，直到 \r\n\r\n
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
      part_ct = it_ct->second;
    }

    // part 内容以“CRLF + 下一个 boundary”结束，正文本身允许包含任意二进制数据。
    std::string marker = "\r\n" + dash_boundary;
    size_t next = body.find(marker, pos);
    if (next == std::string::npos) {
      // 最后一个 part 也应该有 marker
      if (error) {
        *error = "Invalid multipart: missing next boundary";
      }
      return nullptr;
    }

    std::string part_data = body.substr(pos, next - pos);
    pos = next + 2; // 指向 --boundary

    if (!filename.empty()) {
      // 带 filename 的 part 视为文件上传。
      UploadedFile f;
      f.field_name = name;
      f.filename = filename;
      f.content_type = part_ct;
      f.data = std::move(part_data);
      out->files_.push_back(std::move(f));
    } else {
      // 普通字段直接按 name 存储原始值。
      out->fields_[name] = std::move(part_data);
    }

    // pos 当前应该是 --boundary
    size_t bpos = body.find(dash_boundary, pos);
    if (bpos == std::string::npos) {
      break;
    }
    pos = bpos;
  }

  return out;
}

} // namespace zhttp
