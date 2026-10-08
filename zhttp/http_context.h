#ifndef ZHTTP_HTTP_CONTEXT_H_
#define ZHTTP_HTTP_CONTEXT_H_

#include "zhttp/content/parsed_request_body.h"
#include "zhttp/http_request.h"
#include "zhttp/http_response.h"
#include "zhttp/websocket/websocket_handler.h"
#include <functional>
#include <string>
#include <nlohmann/json.hpp>
namespace zhttp {
/** 连接地址及 TLS 信息；由网络层填充，不从 HTTP 头部推断。 */
struct ConnectionInfo {
    std::string remote_address;
    std::string local_address;
    bool tls = false;
};

class Session;
/** 请求终态：写出完成、失败、取消或协议升级。 */
enum class CompletionResult { Completed, Failed, Cancelled, Upgraded };
/**
 * 一次同步请求处理的上下文，拥有响应并共享只读请求。
 *
 * 路由、中间件和完成回调使用同一对象；不可保存引用供请求结束后使用。
 * 派生正文和 Cookie 采用惰性缓存，访问应在当前请求处理流程内完成。
 */
class HttpContext {
  public:
    using ptr = std::shared_ptr<HttpContext>;
    using Params = HttpRequest::Params;
    using Json = nlohmann::json;
    /**
     * 绑定非空只读请求，并初始化响应版本与保持连接标志。
     *
     * @param request 共享请求快照，不能为 nullptr。
     * @param info 连接元信息。
     * @throws std::invalid_argument request 为空。
     */
    explicit HttpContext(std::shared_ptr<const HttpRequest> request,
                         ConnectionInfo info = {});
    /** 未完成请求按 Cancelled 收尾；完成回调只执行一次。 */
    ~HttpContext();
    /** 返回只读请求引用。 */
    const HttpRequest &request() const { return *request_; }
    /** 返回共享的只读请求，可独立延长请求模型生命周期。 */
    std::shared_ptr<const HttpRequest> request_ptr() const { return request_; }
    /** 返回当前可构造的响应。 */
    HttpResponse &response() { return response_; }
    /** 返回当前响应的只读视图。 */
    const HttpResponse &response() const { return response_; }
    /** 返回连接元信息。 */
    const ConnectionInfo &connection_info() const { return connection_; }
    /** 返回请求方法。 */
    HttpMethod method() const { return request().method(); }
    /** 返回请求 HTTP 版本。 */
    HttpVersion version() const { return request().version(); }
    /** 返回请求路径。 */
    const std::string &path() const { return request().path(); }
    /** 返回原始查询串，不含问号。 */
    const std::string &query() const { return request().query(); }
    /** 返回请求内存正文。 */
    const std::string &body() const { return request().body(); }
    /** 返回首个同名头部值；字段名忽略大小写，缺失时返回 fallback。 */
    std::string header(const std::string &key,
                       const std::string &fallback = "") const {
        return request().header(key, fallback);
    }
    /** 返回请求 Content-Type。 */
    std::string content_type() const { return request().content_type(); }
    /** 返回请求是否要求保持连接。 */
    bool is_keep_alive() const { return request().is_keep_alive(); }
    /** 返回网络层提供的对端地址；离线处理时可为空。 */
    const std::string &remote_addr() const {
        return connection_.remote_address;
    }
    /** 接管本次路由匹配的路径参数。 */
    void set_path_params(Params params) { path_params_ = std::move(params); }
    /** 返回已匹配的路径参数集合。 */
    const Params &path_params() const { return path_params_; }
    /** 返回路径参数值；缺失时返回 fallback。 */
    std::string path_param(const std::string &key,
                           const std::string &fallback = "") const;
    /** 返回查询参数首个解码值；缺失时返回 fallback。 */
    std::string query_param(const std::string &key,
                            const std::string &fallback = "") const {
        return request().query_param(key, fallback);
    }
    /** 惰性解析首个 Cookie 头并返回缓存。 */
    const Params &cookies() const;
    /** 返回 Cookie 值；缺失时返回 fallback。 */
    std::string cookie(const std::string &key,
                       const std::string &fallback = "") const;
    /** 返回中间件绑定的会话；没有会话时返回 nullptr。 */
    std::shared_ptr<Session> session() const { return session_; }
    /** 绑定会话；传入 nullptr 可清除绑定。 */
    void set_session(std::shared_ptr<Session> session) {
        session_ = std::move(session);
    }
    /** 依据 Content-Type 判断是否为 JSON。 */
    bool is_json() const { return ParsedRequestBody::is_json(content_type()); }
    /** 解析并缓存 JSON；首次类型不符时视为无需解析，JSON 解析失败返回 false。 */
    bool parse_json() { return parsed_.parse_json(content_type()); }
    /** 惰性获取 JSON 缓存；类型不符或解析失败返回 nullptr。 */
    const Json *json() const { return parsed_.json(content_type()); }
    /** 返回最近 JSON 解析错误文本。 */
    const std::string &json_error() const { return parsed_.json_error(); }
    /** 判断 Content-Type 是否为 URL 编码表单。 */
    bool is_form_urlencoded() const {
        return ParsedRequestBody::is_form_urlencoded(content_type());
    }
    /** 解析并缓存 URL 编码表单；类型不符时视为无需解析并返回 true。 */
    bool parse_form_urlencoded() {
        return parsed_.parse_form_urlencoded(content_type());
    }
    /** 返回惰性解析后的表单缓存；重复键保留最后值。 */
    const Params &form_params() const {
        return parsed_.form_params(content_type());
    }
    /** 返回表单参数；缺失时返回 fallback。 */
    std::string form_param(const std::string &key,
                           const std::string &fallback = "") const;
    /** 判断 Content-Type 是否为 multipart/form-data。 */
    bool is_multipart() const {
        return ParsedRequestBody::is_multipart(content_type());
    }
    /** 解析并缓存 multipart；失败返回 false，可读取 multipart_error()。 */
    bool parse_multipart();
    /** 返回惰性解析的 multipart 缓存；失败返回 nullptr。 */
    const MultipartFormData *multipart() const;
    /** 返回最近 multipart 解析错误文本。 */
    const std::string &multipart_error() const {
        return parsed_.multipart_error();
    }
    /**
     * 设置 WebSocket 升级意图，握手校验与协议切换由网络层完成。
     *
     * @param callbacks 新协议的生命周期回调。
     * @param options 消息大小、子协议和关闭超时。
     * @throws std::logic_error 响应已提交。
     */
    void upgrade_to_websocket(WebSocketCallbacks callbacks,
                              const WebSocketOptions &options = {});
    /** 返回借用的升级意图指针；未请求升级时返回 nullptr。 */
    const WebSocketUpgrade *upgrade() const { return upgrade_.get(); }
    /** 转移升级意图所有权并清除当前记录，供协议层使用。 */
    std::unique_ptr<WebSocketUpgrade> take_upgrade() {
        return std::move(upgrade_);
    }
    /** 清除升级意图并重置响应，默认关闭连接，供异常处理使用。 */
    void reset_result();
    /**
     * 注册请求完成回调，按注册顺序且最多执行一次。
     *
     * @param callback 接收终态的回调，异常被隔离，空回调忽略。
     * @throws std::logic_error 请求已经完成。
     */
    void on_complete(std::function<void(CompletionResult)> callback);
    /** 以指定终态调用全部完成回调；重复调用忽略，异常不向外传播。 */
    void complete(CompletionResult result);
    /** 返回请求是否已经收尾。 */
    bool completed() const { return completed_; }

  protected:
    // Context owns the request snapshot; test builders can rebuild before
    // execution.
    void invalidate_derived();
    ConnectionInfo connection_;

  private:
    std::shared_ptr<const HttpRequest> request_;
    HttpResponse response_;
    ParsedRequestBody parsed_;
    Params path_params_;
    mutable Params cookies_;
    mutable bool cookies_parsed_ = false;
    std::shared_ptr<Session> session_;
    std::unique_ptr<WebSocketUpgrade> upgrade_;
    std::vector<std::function<void(CompletionResult)>> completion_callbacks_;
    bool completed_ = false;
};
} // namespace zhttp

#endif // ZHTTP_HTTP_CONTEXT_H_
