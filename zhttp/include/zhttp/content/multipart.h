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
/*
 * multipart/form-data 请求体格式
 *
 * 适用于 HTTP 表单提交，尤其是同时上传文件和普通表单字段的场景。
 *
 * Content-Type: multipart/form-data; boundary=----WebKitFormBoundaryABC123
 *
 * 请求体格式：
 *
 * ------WebKitFormBoundaryABC123\r\n
 * Content-Disposition: form-data; name="username"\r\n
 * \r\n
 * zhangsan\r\n
 * ------WebKitFormBoundaryABC123\r\n
 * Content-Disposition: form-data; name="file"; filename="test.txt"\r\n
 * Content-Type: text/plain\r\n
 * \r\n
 * Hello World!\r\n
 * ------WebKitFormBoundaryABC123--\r\n
 *
 * 格式说明：
 *
 * 1. Boundary（分隔符）
 *    - 由 Content-Type 请求头中的 boundary 参数指定。
 *    - 用于分隔请求体中的不同表单部分。
 *    - 每个分隔符前面需要添加两个连字符 "--"。
 *    - 最后一个分隔符以两个额外的连字符 "--" 结束。
 *    - boundary 参数本身不包含开头的两个连字符。
 *
 * 2. Content-Disposition
 *    - 每个表单部分通常都包含该头部。
 *    - name：表单字段名称。
 *    - filename：上传文件的原始文件名，可选。
 *
 * 3. Content-Type
 *    - 可选，用于描述当前部分的数据类型。
 *    - 文件部分通常会携带该字段，例如 application/octet-stream。
 *    - 普通文本字段可以不指定。
 *
 * 4. 空行（\r\n）
 *    - 每个部分的头部结束后，需要一个空行。
 *    - 空行用于分隔头部与实际数据。
 *
 * 5. Part Body（部分数据）
 *    - 可以是普通文本、文件内容或其他二进制数据。
 *    - 文件内容可以包含任意二进制字节，不应按文本字符串处理。
 *
 * 6. 结束标记
 *    - 最后一个 Boundary 后追加 "--"。
 *    - 表示整个 multipart/form-data 请求体结束。
 *
 * 注意：
 *    - 各部分之间使用 CRLF（\r\n）分隔。
 *    - Boundary 必须与 Content-Type 中声明的值一致。
 *    - Boundary 不得出现在各部分内容中，形成无法区分的有效分隔符。
 *    - 请求体长度由 HTTP 消息定界机制确定，例如 Content-Length
 *      或适用协议版本中的传输分帧机制。
 *    - multipart/form-data 是请求体的媒体类型，不是 HTTP 传输编码。
 */


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
