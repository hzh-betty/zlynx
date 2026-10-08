#pragma once
#include <string>
#include <utility>
#include <vector>
namespace zhttp {
// 保序且保留重复字段的值对象；验证和大小写查询必须共享同一字段集合。
class HttpHeaders {
  public:
    using Field = std::pair<std::string, std::string>;
    using Fields = std::vector<Field>;
    using const_iterator = Fields::const_iterator;
    static bool valid(const std::string &name, const std::string &value);
    static bool equal_name(const std::string &a, const std::string &b);
    void set(const std::string &name, const std::string &value);
    void append(const std::string &name, const std::string &value);
    std::string get(const std::string &name,
                    const std::string &fallback = "") const;
    std::vector<std::string> get_all(const std::string &name) const;
    void erase(const std::string &name);
    const_iterator find(const std::string &name) const;
    const_iterator begin() const { return fields_.begin(); }
    const_iterator end() const { return fields_.end(); }
    std::size_t count(const std::string &name) const {
        return get_all(name).size();
    }
    std::size_t size() const { return fields_.size(); }
    bool empty() const { return fields_.empty(); }
    void clear() { fields_.clear(); }
    const std::string &at(const std::string &name) const;

  private:
    Fields fields_;
};
} // namespace zhttp
