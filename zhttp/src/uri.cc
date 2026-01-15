#include "uri.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_map>

namespace zhttp {

Uri::Uri() = default;

Uri::ptr Uri::create(const std::string &uri) {
  auto ptr = std::make_shared<Uri>();
  if (!ptr->parse(uri)) {
    return nullptr;
  }
  return ptr;
}

bool Uri::parse(const std::string &uri) {
  if (uri.empty()) {
    return false;
  }

  size_t pos = 0;
  size_t len = uri.length();

  // 1. 解析 scheme
  size_t scheme_end = uri.find("://");
  if (scheme_end != std::string::npos) {
    scheme_ = uri.substr(0, scheme_end);
    // 转换为小写
    std::transform(scheme_.begin(), scheme_.end(), scheme_.begin(), ::tolower);
    pos = scheme_end + 3;
  }

  // 2. 解析 authority (userinfo@host:port)
  size_t path_start = uri.find('/', pos);
  size_t query_start = uri.find('?', pos);
  size_t fragment_start = uri.find('#', pos);

  size_t authority_end = std::min({path_start, query_start, fragment_start});
  if (authority_end == std::string::npos) {
    authority_end = len;
  }

  std::string authority = uri.substr(pos, authority_end - pos);

  // 解析 userinfo
  size_t at_pos = authority.find('@');
  size_t host_start = 0;
  if (at_pos != std::string::npos) {
    userinfo_ = authority.substr(0, at_pos);
    host_start = at_pos + 1;
  }

  // 解析 host 和 port
  std::string host_port = authority.substr(host_start);

  // 检查是否是 IPv6 地址 [::1]
  if (!host_port.empty() && host_port[0] == '[') {
    size_t bracket_end = host_port.find(']');
    if (bracket_end != std::string::npos) {
      host_ = host_port.substr(1, bracket_end - 1);
      if (bracket_end + 1 < host_port.length() &&
          host_port[bracket_end + 1] == ':') {
        try {
          port_ = std::stoi(host_port.substr(bracket_end + 2));
        } catch (...) {
          return false;
        }
      }
    }
  } else {
    size_t colon_pos = host_port.rfind(':');
    if (colon_pos != std::string::npos) {
      host_ = host_port.substr(0, colon_pos);
      try {
        port_ = std::stoi(host_port.substr(colon_pos + 1));
      } catch (...) {
        return false;
      }
    } else {
      host_ = host_port;
    }
  }

  pos = authority_end;

  // 3. 解析 path
  if (pos < len && uri[pos] == '/') {
    size_t path_end = std::min(query_start, fragment_start);
    if (path_end == std::string::npos) {
      path_end = len;
    }
    path_ = uri.substr(pos, path_end - pos);
    pos = path_end;
  }

  // 4. 解析 query
  if (pos < len && uri[pos] == '?') {
    size_t query_end = fragment_start;
    if (query_end == std::string::npos) {
      query_end = len;
    }
    query_ = uri.substr(pos + 1, query_end - pos - 1);
    pos = query_end;
  }

  // 5. 解析 fragment
  if (pos < len && uri[pos] == '#') {
    fragment_ = uri.substr(pos + 1);
  }

  return true;
}

const std::string &Uri::path() const {
  static std::string default_path = "/";
  return path_.empty() ? default_path : path_;
}

int32_t Uri::port() const {
  if (port_ != 0) {
    return port_;
  }
  // 返回默认端口
  if (scheme_ == "http" || scheme_ == "ws") {
    return 80;
  }
  if (scheme_ == "https" || scheme_ == "wss") {
    return 443;
  }
  if (scheme_ == "ftp") {
    return 21;
  }
  return 0;
}

bool Uri::is_default_port() const {
  if (port_ == 0) {
    return true;
  }
  if ((scheme_ == "http" || scheme_ == "ws") && port_ == 80) {
    return true;
  }
  if ((scheme_ == "https" || scheme_ == "wss") && port_ == 443) {
    return true;
  }
  return false;
}

std::string Uri::authority() const {
  std::ostringstream ss;
  if (!userinfo_.empty()) {
    ss << userinfo_ << "@";
  }
  ss << host_;
  if (!is_default_port() && port_ != 0) {
    ss << ":" << port_;
  }
  return ss.str();
}

std::string Uri::host_port() const {
  std::ostringstream ss;
  ss << host_;
  if (!is_default_port() && port_ != 0) {
    ss << ":" << port_;
  }
  return ss.str();
}

std::string Uri::to_string() const {
  std::ostringstream ss;

  if (!scheme_.empty()) {
    ss << scheme_ << "://";
  }

  if (!userinfo_.empty()) {
    ss << userinfo_ << "@";
  }

  // IPv6 地址需要加方括号
  if (host_.find(':') != std::string::npos) {
    ss << "[" << host_ << "]";
  } else {
    ss << host_;
  }

  if (!is_default_port() && port_ != 0) {
    ss << ":" << port_;
  }

  ss << path();

  if (!query_.empty()) {
    ss << "?" << query_;
  }

  if (!fragment_.empty()) {
    ss << "#" << fragment_;
  }

  return ss.str();
}

std::string Uri::url_decode(const std::string &str) {
  std::ostringstream decoded;
  for (size_t i = 0; i < str.length(); ++i) {
    if (str[i] == '%' && i + 2 < str.length()) {
      int value = 0;
      std::istringstream iss(str.substr(i + 1, 2));
      if (iss >> std::hex >> value) {
        decoded << static_cast<char>(value);
        i += 2;
        continue;
      }
    } else if (str[i] == '+') {
      decoded << ' ';
      continue;
    }
    decoded << str[i];
  }
  return decoded.str();
}

std::unordered_map<std::string, std::string> Uri::parse_query() const {
  std::unordered_map<std::string, std::string> params;
  if (query_.empty()) {
    return params;
  }

  std::istringstream iss(query_);
  std::string pair;
  while (std::getline(iss, pair, '&')) {
    size_t eq_pos = pair.find('=');
    if (eq_pos != std::string::npos) {
      std::string key = url_decode(pair.substr(0, eq_pos));
      std::string value = url_decode(pair.substr(eq_pos + 1));
      params[key] = value;
    } else {
      params[url_decode(pair)] = "";
    }
  }

  return params;
}

std::string Uri::query_param(const std::string &key,
                             const std::string &default_value) const {
  auto params = parse_query();
  auto it = params.find(key);
  return it != params.end() ? it->second : default_value;
}

} // namespace zhttp
