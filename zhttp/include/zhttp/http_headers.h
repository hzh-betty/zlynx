#ifndef ZHTTP_HTTP_HEADERS_H_
#define ZHTTP_HTTP_HEADERS_H_

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zhttp {
// 保序且保留重复字段的值对象；验证和大小写查询必须共享同一字段集合。
/**
 * @brief 保序、保留重复字段的 HTTP 头部集合。
 *
 * 字段名按 ASCII 忽略大小写比较，字段值保持原样；修改可能使迭代器和引用失效。
 */
class HttpHeaders {
  public:
    using Field = std::pair<std::string, std::string>;
    using Fields = std::vector<Field>;
    using const_iterator = Fields::const_iterator;

    /**
     * @brief 检查字段名是否为非空 token，字段值是否不含禁止的控制字符。
     */
    static bool valid(std::string_view name, std::string_view value);

    /**
     * @brief 按 ASCII 忽略大小写比较字段名。
     */
    static bool equal_name(std::string_view a, std::string_view b);

    /**
     * @brief 替换所有同名字段，新字段追加到末尾。
     *
     * @param name 字段名。
     * @param value 字段值。
     * @throws std::invalid_argument 字段名或字段值非法。
     */
    void set(std::string_view name, std::string_view value);

    /**
     * @brief 追加字段，保留已有同名字段。
     *
     * @param name 字段名。
     * @param value 字段值。
     * @throws std::invalid_argument 字段名或字段值非法。
     */
    void append(std::string_view name, std::string_view value);

    /**
     * @brief 返回第一个同名字段值；未找到时返回 fallback。
     */
    std::string get(std::string_view name,
                    std::string_view fallback = "") const;

    /**
     * @brief 借用首个字段值；缺失返回
     * nullopt，字段集合修改或销毁后视图可能失效。
     */
    std::optional<std::string_view> get_view(std::string_view name) const;

    /**
     * @brief 按插入顺序返回所有同名字段值。
     */
    std::vector<std::string> get_all(std::string_view name) const;
    void erase(std::string_view name);

    /**
     * @brief 返回第一个同名字段的迭代器；未找到时返回 end()。
     */
    const_iterator find(std::string_view name) const;

    const_iterator begin() const { return fields_.begin(); }

    const_iterator end() const { return fields_.end(); }

    std::size_t count(std::string_view name) const;

    /**
     * @brief 返回总字段数量，包含重复字段。
     */
    std::size_t size() const { return fields_.size(); }

    bool empty() const { return fields_.empty(); }

    void clear() { fields_.clear(); }

    /**
     * @brief 返回第一个同名字段值的只读引用。
     *
     * @param name 字段名，忽略大小写。
     * @return 字段值引用。
     * @throws std::out_of_range 字段不存在。
     */
    const std::string &at(std::string_view name) const;

  private:
    Fields fields_;
};
} // namespace zhttp

#endif
