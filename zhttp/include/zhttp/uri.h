#ifndef ZHTTP_URI_H_
#define ZHTTP_URI_H_

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zhttp {
/**
 * @brief HTTP 请求目标值对象；保留原文，拆分路径和查询串。
 */
class Uri {
  public:
    /** 
    * @brief 请求目标形式
    *       Origin    : origin-form，常用于请求行，缺省为 /  GET /users?id=1 HTTP/1.1
    *       Absolute  : absolute-form，包含 scheme、authority 和 path  GET http://example.com/users?id=1 HTTP/1.1
    *       Authority : authority-form，包含 host 和可选 port  CONNECT example.com:443 HTTP/1.1
    *       Asterisk  : asterisk-form，单独的 *，仅用于 OPTIONS  OPTIONS * HTTP/1.1
    */
    enum class Form { Origin, Absolute, Authority, Asterisk };

    /**
     * @brief 解析并校验请求目标，不对路径做百分号解码。
     *
     * @param target 请求目标原文，默认 /。
     * @throws std::invalid_argument 编码或目标形式非法。
     */
    explicit Uri(std::string target = "/");

    const std::string &raw() const { return raw_; }

    /**
     * @brief 返回不含查询串的路径；绝对 URI 提取路径，缺省为 /。
     */
    const std::string &path() const { return path_; }

    /**
     * @brief 返回不含前导问号的原始查询串。
     */
    const std::string &query() const { return query_; }

    Form form() const { return form_; }

    /**
     * @brief 返回指定查询键的第一个解码值；不存在时返回 fallback。
     */
    std::string query_param(std::string_view name,
                            std::string_view fallback = "") const;

    /**
     * @brief 按原顺序返回指定查询键的所有解码值，保留重复键。
     */
    std::vector<std::string> query_values(std::string_view name) const;

    /**
     * @brief 检查百分号编码完整性，并拒绝空白、控制字符和片段标记。
     */
    static bool valid_encoding(std::string_view value);

  private:
    std::string raw_, path_, query_;
    Form form_ = Form::Origin;
};
} // namespace zhttp

#endif
