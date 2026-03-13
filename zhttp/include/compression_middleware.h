#ifndef ZHTTP_COMPRESSION_MIDDLEWARE_H_
#define ZHTTP_COMPRESSION_MIDDLEWARE_H_

#include "middleware.h"

#include <string>
#include <vector>

namespace zhttp {

/**
 * @brief 响应压缩中间件（动态压缩）
 * @details
 * 该中间件在响应阶段根据客户端 `Accept-Encoding` 对响应体进行动态压缩。
 * - 在客户端同时支持时优先选择 `br`（brotli），其次 `gzip`；
 * - 仅在响应可压缩且收益合理时执行压缩；
 * - 已带 `Content-Encoding` 的响应不重复压缩。
 */
class CompressionMiddleware : public Middleware {
public:
  /**
   * @brief 压缩中间件配置项
   */
  struct Options {
    Options()
        : enable_gzip(true), enable_br(true), min_compress_size(1024),
          gzip_level(5), brotli_quality(5),
          only_compress_success_response(true),
          compressible_content_types(
              {"text/", "application/json", "application/xml",
               "application/javascript", "application/x-javascript",
               "image/svg+xml"}) {}

    bool enable_gzip; // 是否启用 gzip
    bool enable_br;   // 是否启用 brotli(br)

    // 最小压缩阈值（字节）。过小响应通常压缩收益不足。
    size_t min_compress_size;

    // gzip 压缩等级（1~9），越大压缩率更高但 CPU 开销更大。
    int gzip_level;
    // brotli 质量等级（0~11），越大压缩率更高但 CPU 开销更大。
    int brotli_quality;

    // true 时仅压缩 2xx 响应，避免对错误页做额外 CPU 开销。
    bool only_compress_success_response;

    // 可压缩的 Content-Type 前缀/精确值集合。
    std::vector<std::string> compressible_content_types;
  };

  explicit CompressionMiddleware(Options options = Options());

  /**
   * @brief 前置处理无额外逻辑
   */
  bool before(const HttpRequest::ptr &request, HttpResponse &response) override;

  /**
   * @brief 后置处理：执行响应压缩
   */
  void after(const HttpRequest::ptr &request, HttpResponse &response) override;

private:
  enum class Encoding {
    NONE,
    GZIP,
    BR,
  };

  /**
   * @brief 判断是否可以压缩响应
   * @details
   * 该函数会综合检查：
   * - 请求方法是否允许压缩（例如 HEAD 不压缩）；
   * - 响应状态码是否满足策略（默认仅压缩 2xx）；
   * - 响应是否已带 Content-Encoding（避免重复压缩）；
   * - 响应体长度是否达到最小阈值；
   * - Content-Type 是否在可压缩集合中。
   */
  bool can_compress(const HttpRequest::ptr &request,
                    const HttpResponse &response) const;

  /**
   * @brief 判断响应内容类型是否可压缩
   * @details
   * 支持两种规则：
   * - 以 '/' 结尾的前缀规则（如 text/）；
   * - 精确类型规则（如 application/json）。
   */
  bool is_compressible_content_type(const HttpResponse &response) const;

  /**
   * @brief 协商最佳压缩编码
   * @return Encoding::NONE 表示不支持任何压缩；否则返回首选编码。
   */
  Encoding negotiate_encoding(const HttpRequest::ptr &request) const;

  /**
   * @brief 判断 Accept-Encoding 头是否包含特定编码 token
   * @param accept_encoding 原始 Accept-Encoding 头值
   * @param token 待匹配的编码 token（例如 "gzip"）
   * @return true 表示包含；false 表示不包含
   * @details 匹配时会忽略大小写并处理逗号分隔的多个编码值。
   */
  bool has_encoding_token(const std::string &accept_encoding,
                          const std::string &token) const;

  /**
   * @brief 使用 gzip 压缩数据
   * @param input 待压缩的输入数据
   * @param output 压缩后的输出数据
   * @return true 表示压缩成功；false 表示失败
   */
  bool compress_with_gzip(const std::string &input, std::string &output) const;

  /**
   * @brief 使用 brotli 压缩数据
   * @param input 待压缩的输入数据
   * @param output 压缩后的输出数据
   * @return true 表示压缩成功；false 表示失败
   */
  bool compress_with_brotli(const std::string &input,
                            std::string &output) const;

  /**
   * @brief 向响应追加 Vary: Accept-Encoding（避免重复）
   * @param response 响应对象
   */
  void append_vary_accept_encoding(HttpResponse &response) const;

private:
  Options options_;
};

} // namespace zhttp

#endif // ZHTTP_COMPRESSION_MIDDLEWARE_H_
