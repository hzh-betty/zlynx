#include "compression_middleware.h"

#include "http_common.h"

#include <zlib.h>

#ifdef ZHTTP_USE_BROTLI
#include <brotli/encode.h>
#endif

namespace zhttp {

CompressionMiddleware::CompressionMiddleware(CompressionMiddleware::Options options)
    : options_(std::move(options)) {}

bool CompressionMiddleware::before(const HttpRequest::ptr &, HttpResponse &) {
  return true;
}

bool CompressionMiddleware::has_encoding_token(const std::string &accept_encoding,
                                               const std::string &token) const {
  // 这里采用轻量匹配策略：
  // - 先统一小写；
  // - 再按 ',' 分段；
  // - 对每段取 ';' 前 token 做精确比较。
  // 这样可以兼容 "gzip;q=0.8"、"br, gzip" 等常见写法，
  // 同时避免把 "x-gzip" 误判为 "gzip"。
  const std::string lowered = to_lower(accept_encoding);
  const std::string lowered_token = to_lower(token);
  std::vector<std::string> segments = split_string(lowered, ',');
  for (auto &segment : segments) {
    trim(segment);
    if (segment.empty()) {
      continue;
    }

    size_t semicolon = segment.find(';');
    std::string name =
        (semicolon == std::string::npos) ? segment : segment.substr(0, semicolon);
    trim(name);
    if (name == lowered_token) {
      return true;
    }
  }
  return false;
}

CompressionMiddleware::Encoding
CompressionMiddleware::negotiate_encoding(const HttpRequest::ptr &request) const {
  const std::string accept_encoding = request->header("Accept-Encoding");
  if (accept_encoding.empty()) {
    // 客户端未声明可接受压缩编码时，按 HTTP 语义回退为 identity。
    return Encoding::NONE;
  }

  // 与 drogon 保持一致：在客户端同时声明可接受时，优先 br，再回退 gzip。
  if (options_.enable_br && has_encoding_token(accept_encoding, "br")) {
    return Encoding::BR;
  }
  if (options_.enable_gzip && has_encoding_token(accept_encoding, "gzip")) {
    return Encoding::GZIP;
  }
  return Encoding::NONE;
}

bool CompressionMiddleware::is_compressible_content_type(
    const HttpResponse &response) const {
  auto it = response.headers().find("Content-Type");
  if (it == response.headers().end() || it->second.empty()) {
    // 未显式声明类型时，默认按“可压缩”处理，避免错过纯文本响应。
    return true;
  }

  std::string content_type = to_lower(it->second);
  // 去掉 charset 等参数，仅保留 MIME 主类型。
  size_t semicolon = content_type.find(';');
  if (semicolon != std::string::npos) {
    content_type = content_type.substr(0, semicolon);
    trim(content_type);
  }

  for (const auto &allow : options_.compressible_content_types) {
    std::string rule = to_lower(allow);
    if (rule.empty()) {
      continue;
    }
    // 规则以 '/' 结尾时按前缀匹配（例如 text/），否则按精确值匹配。
    if (rule.back() == '/') {
      if (content_type.compare(0, rule.size(), rule) == 0) {
        return true;
      }
      continue;
    }
    if (content_type == rule) {
      return true;
    }
  }

  return false;
}

bool CompressionMiddleware::can_compress(const HttpRequest::ptr &request,
                                         const HttpResponse &response) const {
  // HEAD 响应不包含实体正文，压缩无意义。
  if (request->method() == HttpMethod::HEAD) {
    return false;
  }

  // 按配置仅压缩 2xx，避免错误页额外开销。
  if (options_.only_compress_success_response) {
    const int code = static_cast<int>(response.status_code());
    if (code < 200 || code >= 300) {
      return false;
    }
  }

  // 已经被上游压缩过则直接跳过，避免重复编码。
  if (response.headers().find("Content-Encoding") != response.headers().end()) {
    return false;
  }

  if (response.body_content().size() < options_.min_compress_size) {
    // 小包体通常压缩率收益不高，且会增加 CPU 与延迟。
    return false;
  }

  return is_compressible_content_type(response);
}

bool CompressionMiddleware::compress_with_gzip(const std::string &input,
                                               std::string &output) const {
  if (input.empty()) {
    output.clear();
    return true;
  }

  // deflateInit2 参数说明：
  // - windowBits = 15 + 16：输出 gzip 封装（而非 raw/zlib 封装）；
  // - level 使用配置值并限制在 1~9 范围。
  int level = options_.gzip_level;
  if (level < 1) {
    level = 1;
  }
  if (level > 9) {
    level = 9;
  }

  z_stream stream;
  // 这里使用显式字段初始化，便于和 zlib 文档参数一一对应。
  stream.zalloc = Z_NULL;
  stream.zfree = Z_NULL;
  stream.opaque = Z_NULL;
  stream.avail_in = 0;
  stream.next_in = Z_NULL;

  if (deflateInit2(&stream,
                   level,
                   Z_DEFLATED,
                   15 + 16,
                   8,
                   Z_DEFAULT_STRATEGY) != Z_OK) {
    return false;
  }

  uLong bound = deflateBound(&stream, static_cast<uLong>(input.size()));
  output.resize(static_cast<size_t>(bound));

  stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
  stream.avail_in = static_cast<uInt>(input.size());
  stream.next_out = reinterpret_cast<Bytef *>(&output[0]);
  stream.avail_out = static_cast<uInt>(output.size());

  int rc = deflate(&stream, Z_FINISH);
  if (rc != Z_STREAM_END) {
    // 压缩流未正常结束，返回失败并清空输出，避免调用方误用半成品数据。
    deflateEnd(&stream);
    output.clear();
    return false;
  }

  output.resize(static_cast<size_t>(stream.total_out));
  deflateEnd(&stream);
  return true;
}

bool CompressionMiddleware::compress_with_brotli(const std::string &input,
                                                 std::string &output) const {
#ifdef ZHTTP_USE_BROTLI
  if (input.empty()) {
    output.clear();
    return true;
  }

  int quality = options_.brotli_quality;
  if (quality < 0) {
    quality = 0;
  }
  if (quality > 11) {
    quality = 11;
  }

  size_t compressed_size = BrotliEncoderMaxCompressedSize(input.size());
  if (compressed_size == 0) {
    // Brotli 估算上界失败（极端输入或库异常），直接视为压缩失败。
    return false;
  }

  output.resize(compressed_size);
  BROTLI_BOOL ok = BrotliEncoderCompress(
      quality,
      BROTLI_DEFAULT_WINDOW,
      BROTLI_MODE_GENERIC,
      input.size(),
      reinterpret_cast<const uint8_t *>(input.data()),
      &compressed_size,
      reinterpret_cast<uint8_t *>(&output[0]));

  if (ok == BROTLI_FALSE) {
    // 与 gzip 一致，失败时清空输出，保证失败语义明确。
    output.clear();
    return false;
  }

  output.resize(compressed_size);
  return true;
#else
  (void)input;
  output.clear();
  return false;
#endif
}

void CompressionMiddleware::append_vary_accept_encoding(
    HttpResponse &response) const {
  auto it = response.headers().find("Vary");
  if (it == response.headers().end()) {
    response.header("Vary", "Accept-Encoding");
    return;
  }

  std::string vary = to_lower(it->second);
  if (vary.find("accept-encoding") != std::string::npos) {
    return;
  }

  std::string new_vary = it->second;
  new_vary += ", Accept-Encoding";
  response.header("Vary", new_vary);
}

void CompressionMiddleware::after(const HttpRequest::ptr &request,
                                  HttpResponse &response) {
  // 压缩主流程：
  // 1) 先判断是否值得压缩；
  // 2) 按协商结果选择 br/gzip；
  // 3) 若 br 失败且客户端支持 gzip，则回退 gzip；
  // 4) 仅在“压缩后更小”时替换响应体，避免负优化。
  if (!can_compress(request, response)) {
    return;
  }

  const Encoding encoding = negotiate_encoding(request);
  if (encoding == Encoding::NONE) {
    return;
  }

  std::string compressed;
  bool success = false;
  if (encoding == Encoding::BR) {
    success = compress_with_brotli(response.body_content(), compressed);
    if (success && !compressed.empty() && compressed.size() < response.body_content().size()) {
      response.body(std::move(compressed));
      response.header("Content-Encoding", "br");
    } else {
      success = false;
    }
  }

  // br 压缩失败时，按协商结果若客户端也支持 gzip，则尝试回退 gzip。
  if (!success && encoding == Encoding::BR && options_.enable_gzip &&
      has_encoding_token(request->header("Accept-Encoding"), "gzip")) {
    if (compress_with_gzip(response.body_content(), compressed) && !compressed.empty() &&
        compressed.size() < response.body_content().size()) {
      response.body(std::move(compressed));
      response.header("Content-Encoding", "gzip");
      success = true;
    }
  }

  if (!success && encoding == Encoding::GZIP) {
    if (compress_with_gzip(response.body_content(), compressed) && !compressed.empty() &&
        compressed.size() < response.body_content().size()) {
      response.body(std::move(compressed));
      response.header("Content-Encoding", "gzip");
      success = true;
    }
  }

  if (!success) {
    // 压缩失败或收益不足时保持原响应，确保功能正确优先于压缩率。
    return;
  }

  // 响应体变化后，保证 Content-Length 与实体长度一致。
  response.header("Content-Length", std::to_string(response.body_content().size()));
  append_vary_accept_encoding(response);
}

} // namespace zhttp
