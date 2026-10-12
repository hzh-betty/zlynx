/**
 * @file multipart.h
 *
 * @author hzh-betty
 */
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
 *
 * multipart/form-data 里的每个文件字段最终都会被表示成一个 UploadedFile。
 * data 保存的是原始字节内容，因此既可以是文本文件，也可以是二进制文件。
 */
struct UploadedFile {
    std::string field_name;   // 表单字段名（name）
    std::string filename;     // 原始文件名
    std::string content_type; // Content-Type（可能为空）
    std::string data;         // 文件内容（原始字节）

    /**
     * @brief 将文件内容写入指定路径，覆盖已有文件。
     * @param[out] error 非空时接收打开或写入失败的说明；成功时不清除原值。
     * @return 打开或 write() 检测到失败时返回 false，否则返回 true。
     * @warning 失败时目标文件可能已被截断或部分写入，不会恢复原文件。
     * @note 不检查关闭时的刷新错误，也不保证数据已持久化到存储设备。
     */
    bool save_to(const std::string &filepath,
                 std::string *error = nullptr) const;
};

/**
 * @brief multipart/form-data 解析结果
 *
 * 普通表单字段会进入 fields()，同名字段保留最后一个值；文件字段会进入 files()。
 * 同名文件可以有多个，file() 只返回第一个匹配项。
 * fields()、files() 和 file()
 * 返回的引用或指针均借用本对象，不延长对象生命周期。
 */
class MultipartFormData {
  public:
    using ptr = std::shared_ptr<MultipartFormData>;
    using Fields = std::unordered_map<std::string, std::string>;

    /**
     * @brief 获取普通文本字段
     *
     * @return 文本字段映射表
     */
    const Fields &fields() const { return fields_; }

    /**
     * @brief 获取上传文件列表
     *
     * @return 所有文件字段解析结果
     */
    const std::vector<UploadedFile> &files() const { return files_; }

    /**
     * @brief 按字段名读取普通字段
     *
     * @param key 字段名
     * @param default_val 未命中时返回的默认值
     * @return 字段值
     */
    std::string field(const std::string &key,
                      const std::string &default_val = "") const;

    /**
     * @brief 按字段名读取第一个上传文件
     *
     * @param field_name 表单字段名
     * @return 借用内部文件元素的指针；未命中时返回
     * nullptr，对象销毁后指针失效。
     */
    const UploadedFile *file(const std::string &field_name) const;

    /**
     * @brief 从 HttpRequest 解析 multipart
     *
     * @param request 已经完成 HTTP 头和 Body 解析的请求对象
     * @param error 可选的错误输出参数
     * @return 非 multipart 请求返回非空的空结果对象；解析成功返回结果，失败返回
     * nullptr。
     * @note 非空 error 在入口清空，并在解析失败时写入原因。
     */
    static ptr parse(const HttpRequest &request, std::string *error = nullptr);

  private:
    // 普通文本字段。
    Fields fields_;

    // 上传文件字段。
    std::vector<UploadedFile> files_;
};

} // namespace zhttp

#endif
