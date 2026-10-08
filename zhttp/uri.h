#pragma once
#include <string>
#include <utility>
#include <vector>
namespace zhttp {
class Uri {
  public:
    enum class Form { Origin, Absolute, Authority, Asterisk };
    explicit Uri(std::string target = "/");
    const std::string &raw() const { return raw_; }
    const std::string &path() const { return path_; }
    const std::string &query() const { return query_; }
    Form form() const { return form_; }
    std::string query_param(const std::string &name,
                            const std::string &fallback = "") const;
    std::vector<std::string> query_values(const std::string &name) const;
    static bool valid_encoding(const std::string &value);

  private:
    std::string raw_, path_, query_;
    Form form_ = Form::Origin;
};
} // namespace zhttp
