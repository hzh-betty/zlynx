#ifndef ZHTTP_MULTIPART_H_
#define ZHTTP_MULTIPART_H_

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace zhttp {

class HttpRequest;

/**
 * @brief 上传文件
 */
struct UploadedFile {
  std::string field_name;   // 表单字段名（name）
  std::string filename;     // 原始文件名
  std::string content_type; // Content-Type（可能为空）
  std::string data;         // 文件内容（原始字节）

  /**
   * @brief 保存到指定文件路径
   * @return true 成功，false 失败
   */
  bool save_to(const std::string &filepath, std::string *error = nullptr) const;
};

/**
 * @brief multipart/form-data 解析结果
 */
class MultipartFormData {
public:
  using ptr = std::shared_ptr<MultipartFormData>;
  using Fields = std::unordered_map<std::string, std::string>;

  const Fields &fields() const { return fields_; }
  const std::vector<UploadedFile> &files() const { return files_; }

  std::string field(const std::string &key,
                    const std::string &default_val = "") const;

  const UploadedFile *file(const std::string &field_name) const;

  /**
   * @brief 从 HttpRequest 解析 multipart
   */
  static ptr parse(const HttpRequest &request, std::string *error = nullptr);

private:
  Fields fields_;
  std::vector<UploadedFile> files_;
};

} // namespace zhttp

#endif // ZHTTP_MULTIPART_H_
