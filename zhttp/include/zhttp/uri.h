#ifndef ZHTTP_URI_H_
#define ZHTTP_URI_H_

#include <string>
#include <utility>
#include <vector>
namespace zhttp {
/** HTTP 请求目标值对象；保留原文，拆分路径和查询串。 */
class Uri {
  public:
    /** 请求目标形式：路径、绝对 URI、CONNECT authority 或星号。 */
    enum class Form { Origin, Absolute, Authority, Asterisk };
    /**
     * 解析并校验请求目标，不对路径做百分号解码。
     *
     * @param target 请求目标原文，默认 /。
     * @throws std::invalid_argument 编码或目标形式非法。
     */
    explicit Uri(std::string target = "/");
    /** 返回请求目标原文。 */
    const std::string &raw() const { return raw_; }
    /** 返回不含查询串的路径；绝对 URI 提取路径，缺省为 /。 */
    const std::string &path() const { return path_; }
    /** 返回不含前导问号的原始查询串。 */
    const std::string &query() const { return query_; }
    /** 返回请求目标形式。 */
    Form form() const { return form_; }
    /** 返回指定查询键的第一个解码值；不存在时返回 fallback。 */
    std::string query_param(const std::string &name,
                            const std::string &fallback = "") const;
    /** 按原顺序返回指定查询键的所有解码值，保留重复键。 */
    std::vector<std::string> query_values(const std::string &name) const;
    /** 检查百分号编码完整性，并拒绝空白、控制字符和片段标记。 */
    static bool valid_encoding(const std::string &value);

  private:
    std::string raw_, path_, query_;
    Form form_ = Form::Origin;
};
} // namespace zhttp

#endif // ZHTTP_URI_H_
