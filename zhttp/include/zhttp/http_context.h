#ifndef ZHTTP_HTTP_CONTEXT_H_
#define ZHTTP_HTTP_CONTEXT_H_

#include "zhttp/detail/parsed_request_body.h"
#include "zhttp/http_request.h"
#include "zhttp/http_response.h"
#include "zhttp/websocket/websocket_handler.h"
#include <chrono>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>

namespace zhttp {
/**
 * @brief 连接地址及 TLS 信息；由网络层填充，不从 HTTP 头部推断。
 */
struct ConnectionInfo {
    std::string remote_address;
    std::string local_address;
    bool tls = false;
};

class Session;

namespace mid {
class TimeoutMiddleware;
}

/**
 * @brief 请求终态：写出完成、失败、取消或协议升级。
 */
enum class CompletionResult { Completed, Failed, Cancelled, Upgraded };

/**
 * @brief 一次同步请求处理的上下文，拥有响应并共享只读请求。
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
     * @brief 绑定非空只读请求，并初始化响应版本与保持连接标志。
     *
     * @param request 共享请求快照，不能为 nullptr。
     * @param info 连接元信息。
     * @throws std::invalid_argument request 为空。
     */
    explicit HttpContext(std::shared_ptr<const HttpRequest> request,
                         ConnectionInfo info = {});

    /**
     * @brief 未完成请求按 Cancelled 收尾；完成回调只执行一次。
     */
    ~HttpContext();

    const HttpRequest &request() const { return *request_; }

    /**
     * @brief 返回共享的只读请求，可独立延长请求模型生命周期。
     */
    std::shared_ptr<const HttpRequest> request_ptr() const { return request_; }

    HttpResponse &response() { return response_; }

    const HttpResponse &response() const { return response_; }

    const ConnectionInfo &connection_info() const { return connection_; }

    HttpMethod method() const { return request().method(); }

    HttpVersion version() const { return request().version(); }

    const std::string &path() const { return request().path(); }

    /**
     * @brief 返回原始查询串，不含问号。
     */
    const std::string &query() const { return request().query(); }

    const std::string &body() const { return request().body(); }

    /**
     * @brief 返回首个同名头部值；字段名忽略大小写，缺失时返回 fallback。
     */
    std::string header(const std::string &key,
                       const std::string &fallback = "") const {
        return request().header(key, fallback);
    }

    /**
     * @brief 借用首个头部值；缺失返回空视图，请求修改或销毁后视图可能失效。
     */
    std::string_view header_view(std::string_view key) const {
        return request().header_view(key);
    }

    std::string content_type() const { return request().content_type(); }

    bool is_keep_alive() const { return request().is_keep_alive(); }

    /**
     * @brief 返回网络层提供的对端地址；离线处理时可为空。
     */
    const std::string &remote_addr() const {
        return connection_.remote_address;
    }

    void set_path_params(Params params) { path_params_ = std::move(params); }

    const Params &path_params() const { return path_params_; }

    /**
     * @brief 返回路径参数值；缺失时返回 fallback。
     */
    std::string path_param(const std::string &key,
                           const std::string &fallback = "") const;

    /**
     * @brief 返回查询参数首个解码值；缺失时返回 fallback。
     */
    std::string query_param(const std::string &key,
                            const std::string &fallback = "") const {
        return request().query_param(key, fallback);
    }

    /**
     * @brief 惰性解析首个 Cookie 头并返回缓存。
     */
    const Params &cookies() const;

    /**
     * @brief 返回 Cookie 值；缺失时返回 fallback。
     */
    std::string cookie(const std::string &key,
                       const std::string &fallback = "") const;

    /**
     * @brief 返回中间件绑定的会话；没有会话时返回 nullptr。
     */
    std::shared_ptr<Session> session() const { return session_; }

    /**
     * @brief 绑定会话；传入 nullptr 可清除绑定。
     */
    void set_session(std::shared_ptr<Session> session) {
        session_ = std::move(session);
    }

    /**
     * @brief 依据 Content-Type 判断是否为 JSON。
     */
    bool is_json() const {
        return detail::ParsedRequestBody::is_json(header_view("Content-Type"));
    }

    /**
     * @brief 解析并缓存 JSON；首次类型不符时视为无需解析，JSON 解析失败返回
     * false。
     */
    bool parse_json() {
        return parsed_.parse_json(header_view("Content-Type"));
    }

    /**
     * @brief 惰性获取 JSON 缓存；类型不符或解析失败返回 nullptr。
     */
    const Json *json() const {
        return parsed_.json(header_view("Content-Type"));
    }

    const std::string &json_error() const { return parsed_.json_error(); }

    /**
     * @brief 判断 Content-Type 是否为 URL 编码表单。
     */
    bool is_form_urlencoded() const {
        return detail::ParsedRequestBody::is_form_urlencoded(
            header_view("Content-Type"));
    }

    /**
     * @brief 解析并缓存 URL 编码表单；类型不符时视为无需解析并返回 true。
     */
    bool parse_form_urlencoded() {
        return parsed_.parse_form_urlencoded(header_view("Content-Type"));
    }

    /**
     * @brief 返回惰性解析后的表单缓存；重复键保留最后值。
     */
    const Params &form_params() const {
        return parsed_.form_params(header_view("Content-Type"));
    }

    /**
     * @brief 返回表单参数；缺失时返回 fallback。
     */
    std::string form_param(const std::string &key,
                           const std::string &fallback = "") const;

    bool is_multipart() const {
        return detail::ParsedRequestBody::is_multipart(
            header_view("Content-Type"));
    }

    /**
     * @brief 解析并缓存 multipart；失败返回 false，可读取 multipart_error()。
     */
    bool parse_multipart();

    /**
     * @brief 返回惰性解析的 multipart 缓存；失败返回 nullptr。
     */
    const MultipartFormData *multipart() const;

    const std::string &multipart_error() const {
        return parsed_.multipart_error();
    }

    /**
     * @brief 设置 WebSocket 升级意图，握手校验与协议切换由网络层完成。
     *
     * @param callbacks 新协议的生命周期回调。
     * @param options 消息大小、子协议和关闭超时。
     * @throws std::logic_error 响应已提交。
     */
    void upgrade_to_websocket(WebSocketCallbacks callbacks,
                              const WebSocketOptions &options = {});

    /**
     * @brief 返回借用的升级意图指针；未请求升级时返回 nullptr。
     */
    const WebSocketUpgrade *upgrade() const { return upgrade_.get(); }

    /**
     * @brief 清除升级意图并重置响应，默认关闭连接，供异常处理使用。
     */
    void reset_result();

    /**
     * @brief 注册请求完成回调，按注册顺序且最多执行一次。
     *
     * @param callback 接收终态的回调，异常被隔离，空回调忽略。
     * @throws std::logic_error 请求已经完成。
     */
    void on_complete(std::function<void(CompletionResult)> callback);

    /**
     * @brief 以指定终态调用全部完成回调；重复调用忽略，异常不向外传播。
     */
    void complete(CompletionResult result);

    bool completed() const { return completed_; }

  protected:
    // Context 持有请求快照；测试构建器可在执行前重建请求，
    // 重建后须清除派生缓存。
    void invalidate_derived();
    ConnectionInfo connection_;

  private:
    friend class HttpProtocolHandler;
    friend class mid::TimeoutMiddleware;

    // 钩子进入记录归当前请求所有；身份指针只用于配对，不拥有或解引用中间件。
    std::vector<std::pair<const mid::TimeoutMiddleware *,
                          std::chrono::steady_clock::time_point>>
        timeout_entries_;
    friend class RequestPipeline;

    std::unique_ptr<WebSocketUpgrade> take_upgrade() {
        return std::move(upgrade_);
    }

    std::shared_ptr<const HttpRequest> request_;
    HttpResponse response_;
    detail::ParsedRequestBody parsed_;
    Params path_params_;
    mutable Params cookies_;
    mutable bool cookies_parsed_ = false;
    std::shared_ptr<Session> session_;
    std::unique_ptr<WebSocketUpgrade> upgrade_;
    std::vector<std::function<void(CompletionResult)>> completion_callbacks_;
    bool completed_ = false;
};
} // namespace zhttp

#endif
