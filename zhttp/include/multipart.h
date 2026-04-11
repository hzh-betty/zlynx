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
 * @details
 * multipart/form-data 里的每个文件字段最终都会被表示成一个 UploadedFile。
 * data 保存的是原始字节内容，因此既可以是文本文件，也可以是二进制文件。
 */
struct UploadedFile {
    std::string field_name;   // 表单字段名（name）
    std::string filename;     // 原始文件名
    std::string content_type; // Content-Type（可能为空）
    std::string data;         // 文件内容（原始字节）

    /**
     * @brief 保存到指定文件路径
     * @param filepath 目标文件路径
     * @param error 可选的错误输出参数
     * @return true 成功，false 失败
     */
    bool save_to(const std::string &filepath,
                 std::string *error = nullptr) const;
};

/**
 * @brief multipart/form-data 解析结果
 * @details
 * 普通表单字段会进入 fields()，文件字段会进入 files()。
 * 一个字段名理论上可以对应多个文件，但当前 file() 便捷接口只返回第一个匹配项。
 */
class MultipartFormData {
  public:
    using ptr = std::shared_ptr<MultipartFormData>;
    using Fields = std::unordered_map<std::string, std::string>;

    /**
     * @brief 获取普通文本字段
     * @return 文本字段映射表
     */
    const Fields &fields() const { return fields_; }

    /**
     * @brief 获取上传文件列表
     * @return 所有文件字段解析结果
     */
    const std::vector<UploadedFile> &files() const { return files_; }

    /**
     * @brief 按字段名读取普通字段
     * @param key 字段名
     * @param default_val 未命中时返回的默认值
     * @return 字段值
     */
    std::string field(const std::string &key,
                      const std::string &default_val = "") const;

    /**
     * @brief 按字段名读取第一个上传文件
     * @param field_name 表单字段名
     * @return 文件指针；未命中时返回 nullptr
     */
    const UploadedFile *file(const std::string &field_name) const;

    /**
     * @brief 从 HttpRequest 解析 multipart
     * @param request 已经完成 HTTP 头和 Body 解析的请求对象
     * @param error 可选的错误输出参数
     * @return 解析成功返回结果对象，失败返回 nullptr
     */
    static ptr parse(const HttpRequest &request, std::string *error = nullptr);

  private:
    // 普通文本字段。
    Fields fields_;

    // 上传文件字段。
    std::vector<UploadedFile> files_;
};

} // namespace zhttp

#endif // ZHTTP_MULTIPART_H_
