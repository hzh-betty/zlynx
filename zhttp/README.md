# zhttp

`zhttp` 是 zlynx 的 C++17 HTTP/HTTPS 与 WebSocket 服务模块，基于 `znet` 的
协程 TCP 服务和 `zlog` 日志模块。它提供路由、中间件、请求体解析、静态文件服务
以及同步响应写出。

## 快速开始

包含统一头文件 `zhttp/zhttp.h`，通过 `HttpServerBuilder` 注册路由：

```cpp
#include "zhttp/zhttp.h"

int main() {
    zhttp::HttpServerBuilder builder;
    builder.listen("0.0.0.0", 8080)
        .threads(4)
        .server_name("demo")
        .get("/health", [](zhttp::HttpContext &context) {
            context.response().json(R"({"ok":true})");
        })
        .get("/users/:id", [](zhttp::HttpContext &context) {
            context.response().text("user=" + context.path_param("id"));
        })
        .post("/echo", [](zhttp::HttpContext &context) {
            const auto *json = context.json();
            if (!json) {
                context.response().status(zhttp::HttpStatus::BAD_REQUEST)
                    .text("Expected application/json");
                return;
            }
            context.response().json(json->dump());
        });
    builder.run();
}
```

`run()` 构建并启动服务，阻塞至收到 SIGINT 或 SIGTERM。`build()` 只构造和初始化
服务器，不启动监听；需要自行管理生命周期时，调用返回对象的 `start()` / `stop()`。
Builder 将线程数与栈模式映射为每个服务器实例的 RuntimeOptions，不修改全局协程配置。
直接构造时使用 `HttpServer(address, options)`，Runtime 在首次 start() 时创建。
也可通过 `HttpServer(address, runtime, application)` 借用已有 Runtime，并在多个
监听器间共享 Application；Runtime 必须比所有监听器活得更久。
start()/stop()/析构由控制线程负责，请求回调使用 request_stop()，随后由控制线程 stop() 等待。
start() 与 set_ssl_certificate() 返回 znet::Result<void>，失败原因通过 error() 获取。
Builder 默认注册请求体解析中间件，按 Content-Type 解析 JSON、URL 编码表单和
multipart。直接构造 `HttpServer` 时，可自行注册 `RequestBodyMiddleware`，也可通过
Context 的访问接口惰性解析正文。

```bash
curl http://127.0.0.1:8080/health
curl http://127.0.0.1:8080/users/42
curl -H 'Content-Type: application/json' \
  -d '{"message":"hello"}' http://127.0.0.1:8080/echo
```

## 构建与接入

构建需要 CMake 3.18+、C++17 编译器、POSIX/Linux 接口，以及以下依赖：

| 依赖 | 用途 |
|---|---|
| `znet`、`zlog`、Threads | 网络运行时与日志 |
| OpenSSL | TLS 与 WebSocket 握手 |
| fmt | 日志格式化 |
| ZLIB、Brotli encoder (`brotlienc`) | gzip / br 压缩 |
| nlohmann_json | JSON 请求体解析 |
| 提供 `<toml.hpp>` 的 TOML 库 | 配置文件解析 |

仓库预设使用 Clang 和 Ninja，并要求 CMake 支持 `CMakePresets.json` 的版本 6。
在仓库根目录构建：

```bash
cmake --preset debug
cmake --build --preset debug -j 4
```

不使用预设时：

```bash
cmake -S . -B build/debug -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON
cmake --build build/debug -j 4
```

源码树内及安装后的完整应用链接 `zhttp::zhttp`；离线 HTTP 处理链接 `zhttp::core`，
核心库不依赖 znet、zco 或 zlog。安装后通过 CMake 包接入：

```bash
cmake --preset release
cmake --build --preset release -j 4
cmake --install build/release --prefix "$PWD/install"
```

应用的 `CMakeLists.txt`：

```cmake
cmake_minimum_required(VERSION 3.18)
project(zhttp_demo LANGUAGES CXX)

find_package(zhttp CONFIG REQUIRED)
add_executable(zhttp_demo main.cc)
target_link_libraries(zhttp_demo PRIVATE zhttp::zhttp)
```

配置应用时将安装前缀传给 `CMAKE_PREFIX_PATH`。模块安装在
`<prefix>/include/zhttp`，包配置位于 `<prefix>/<libdir>/cmake/zhttp`；公共依赖的
头文件和编译要求由导出目标传播。使用自定义安装目录运行共享库应用时，将
`<prefix>/<libdir>` 加入动态链接器搜索路径，例如通过 `LD_LIBRARY_PATH` 设置。

## 请求与响应

业务处理器接收 `HttpContext &`。Context 持有当前响应、只读请求，以及路径参数、
Cookie、会话和正文解析缓存。

| 接口 | 行为 |
|---|---|
| `context.request()` | 读取方法、版本、URI、头部、trailer 和正文 |
| `context.path_param(key, fallback)` | 读取路由捕获的路径参数 |
| `context.query_param(key, fallback)` | 读取首个同名查询参数，解码百分号编码和 `+` |
| `context.request().query_values(key)` | 按顺序读取全部同名查询参数 |
| `context.header(key, fallback)` | 忽略字段名大小写，读取首个同名头部 |
| `context.request().headers().get_all(key)` | 读取全部同名头部，保留重复项 |
| `context.cookie(key, fallback)` | 惰性解析 Cookie 并读取值 |
| `context.json()` | 获取 JSON 缓存，类型不符或解析失败时为 `nullptr` |
| `context.form_param(key, fallback)` | 读取 URL 编码表单，重复键取最后值 |
| `context.multipart()` | 获取 multipart 解析结果，失败时为 `nullptr` |
| `context.session()` | 获取会话中间件绑定的会话，没有会话时为 `nullptr` |

`MultipartFormData::fields()` 保存普通字段，`files()` 保存上传文件。上传文件的
内容保存在内存中，通过 `UploadedFile::save_to()` 写入指定路径。

`HttpResponse` 支持链式设置状态、头部和正文。`json()` 接收已经序列化的 JSON
字符串；`html()` 和 `text()` 设置对应的 UTF-8 内容类型。`redirect(url, code)`
构造普通重定向，默认状态为 302。Cookie 通过 `set_cookie()` / `delete_cookie()`
操作，多个 Set-Cookie 值独立保留。

`HttpBody` 是不可复制、可移动的正文数据源，支持内存、文件和同步流。文件正文
在创建时打开文件并持有描述符，可指定字节偏移和长度：

```cpp
#include "zhttp/zhttp.h"

void download(zhttp::HttpContext &context) {
    context.response().content_type("application/octet-stream")
        .body(zhttp::HttpBody::file("report.bin"));
}
```

响应写出器统一处理 Content-Length、Transfer-Encoding、HEAD 和无正文状态码。
发送头部前响应会被提交，此后修改响应的操作会抛出 `std::logic_error`。

### 同步流与完成通知

同步流回调填充框架提供的缓冲区，返回实际字节数，返回 0 表示结束：

```cpp
#include "zhttp/zhttp.h"
#include <cstring>

void stream_hello(zhttp::HttpContext &context) {
    context.response().content_type("text/plain")
        .stream([sent = false](char *buffer, std::size_t capacity) mutable
                    -> std::size_t {
            if (sent || capacity < 5)
                return 0;
            std::memcpy(buffer, "hello", 5);
            sent = true;
            return 5;
        }, 5);
}
```

第二个参数是总字节数，省略时长度未知。HTTP/1.1 对未知长度使用 chunked；
HTTP/1.0 通过关闭连接界定正文结束。回调在当前连接处理过程中同步执行，每批
数据排空后再拉取下一批。回调抛出异常、产出超过缓冲区容量、与声明长度不符或
发送失败时，写出失败并关闭连接。

`context.on_complete(callback)` 按注册顺序通知请求终态：`Completed`、`Failed`、
`Cancelled` 或 `Upgraded`。完成回调最多执行一次，单个回调的异常不会阻止其他
回调。Context 及其借用引用只在本次请求处理期间有效；流回调不能保留缓冲区、
跨线程写出，或无限等待外部生产者。

## 路由与中间件

Builder 提供 `get()`、`post()`、`put()`、`del()` 和 `websocket()`。通过
`server->router().add_route(method, path, handler)` 可注册其他 HTTP 方法；
`add_regex_route()` 支持正则路径和捕获组参数名。

路径支持静态字符串、`:name` 参数和 `*name` 通配符。普通路由按静态路径、动态
路径、正则路由的顺序匹配；配置首页后，GET/HEAD 的 `/` 和 `/home` 会先处理
首页跳转。处理器既可以是函数或 lambda，也可以是派生自 `RouteHandler` 的共享对象。

中间件使用 `zhttp::mid` 命名空间，也可通过 `zhttp::Middleware` 引用基类：

```cpp
#include "zhttp/zhttp.h"

class ServiceHeader : public zhttp::Middleware {
  public:
    void after(zhttp::HttpContext &context) override {
        context.response().header("X-Service", "demo");
    }
};

std::shared_ptr<zhttp::HttpServer> make_server() {
    zhttp::HttpServerBuilder builder;
    builder.get("/api/ping", [](zhttp::HttpContext &context) {
        context.response().text("pong");
    });
    auto server = builder.build();
    server->use(std::make_shared<ServiceHeader>());             // 全局
    server->use("/api/ping", std::make_shared<ServiceHeader>()); // 精确请求路径
    server->use_group("/api", std::make_shared<ServiceHeader>()); // 前缀子路径
    return server;
}
```

路径组匹配前缀下的子路径，`/api` 组匹配 `/api/ping`，不匹配 `/api` 或 `/apiv1`。
组中间件仅在路由命中时加入处理链。选择顺序为全局、从外到内的路径组、精确路径。

`before()` 按顺序执行。返回 false 会跳过后续中间件和业务处理器；正常返回过
`before()` 的中间件仍逆序执行 `after()`，包括返回 false 的中间件。抛出异常的
`before()` 不执行自身 `after()`；某个 `after()` 失败不影响其余中间件收尾。
可通过 Builder 的 `not_found()` / `exception_handler()` 或 Server 对应接口设置
404 与异常响应，默认异常响应为 500。

路由、处理链和服务器参数应在 `start()` 前配置。`start()` 会冻结配置，即使启动
监听失败也不能继续修改。共享的路由处理器和中间件可能被多个请求并发调用。

### 内置中间件

| 中间件 | 用途 |
|---|---|
| `RequestBodyMiddleware` | JSON、URL 编码表单、multipart 解析及失败响应 |
| `AuthenticationMiddleware` | Session 或 Bearer Token 认证 |
| `RoleAuthorizationMiddleware` | 按所需角色授权 |
| `SessionMiddleware` | 从 Cookie 恢复会话，保存修改并更新 Cookie |
| `CorsMiddleware` | 跨域响应头与 OPTIONS 预检 |
| `CompressionMiddleware` | gzip / br 动态响应压缩 |
| `StaticFileMiddleware` | 静态资源、目录索引、条件请求、单范围与预压缩文件 |
| `RateLimiterMiddleware` | 令牌桶、固定窗口或滑动窗口限流 |
| `TimeoutMiddleware` | 业务返回后检查耗时并按配置改写响应，不抢占业务执行 |
| `ErrorMiddleware` | 统一格式化 4xx / 5xx 响应 |
| `SecurityMiddleware` | 补充缺失的安全响应头，HSTS 默认关闭 |

各中间件通过 `Options` 配置，详见 [middleware](include/zhttp/middleware/) 的头文件。
`SessionManager` 使用带滑动过期的进程内存存储，按操作次数清理过期项；会话数据
不跨进程持久化，返回的请求级 `Session` 不支持跨线程共享读写。

静态文件支持 ETag、Last-Modified、GET/HEAD 单个 bytes Range、小文件内存缓存
以及 `.br` / `.gz` 预压缩资源。动态压缩与静态文件共用 Accept-Encoding 协商规则：
按 q 权重选择，同权重优先 br 再 gzip；未显式声明的 identity 作为最后回退。
客户端禁止 identity 且无可用编码时返回 406。

## HTTPS 与 WebSocket

HTTPS 在当前监听地址上启用 TLS：

```cpp
#include "zhttp/zhttp.h"

int main() {
    zhttp::HttpServerBuilder builder;
    builder.listen("0.0.0.0", 8443)
        .enable_https("server.crt", "server.key")
        .get("/health", [](zhttp::HttpContext &context) {
            context.response().text("ok");
        });
    builder.run();
}
```

启用 HTTPS 需要 PEM 证书和私钥。框架不创建 HTTP → HTTPS 强制重定向监听器。

WebSocket 路由接收完整文本或二进制消息，以下示例回显消息：

```cpp
#include "zhttp/zhttp.h"

int main() {
    zhttp::WebSocketCallbacks callbacks;
    callbacks.on_message = [](const zhttp::WebSocketConnection::ptr &connection,
                              std::string &&message,
                              zhttp::WebSocketMessageType type) {
        if (type == zhttp::WebSocketMessageType::kText)
            connection->send_text(message);
        else
            connection->send_binary(message);
    };
    zhttp::WebSocketOptions options;
    options.max_message_size = 16 * 1024 * 1024;
    options.close_timeout_ms = 5000;

    zhttp::HttpServerBuilder builder;
    builder.listen("0.0.0.0", 8080).websocket("/ws", callbacks, options);
    builder.run();
}
```

`on_open` 在 101 握手写出并切换协议后触发；`on_message` 接收已组装并校验的完整
消息；`on_error` 通知错误，`on_close` 最多通知一次关闭事件。服务器按
`WebSocketOptions::subprotocols` 的顺序选择客户端也支持的子协议。

发送、ping/pong 和主动关闭应在连接回调内执行。数据发送接口单次载荷上限为
1 MiB；ping/pong 载荷最多 125 字节。发送成功表示用户态输出缓冲区已排空，
不表示对端已收到。主动关闭进入 Closing 状态并等待对端关闭帧，默认超时 5 秒。
消息大小和关闭超时配置为 0 时使用默认值。

## 限制与超时

HTTP 解析器默认使用以下 `RequestLimits`：

| 限制 | 默认值 |
|---|---:|
| 请求行（含 CRLF） | 8 KiB |
| 头部与 trailer 累计字节数（含 CRLF） | 64 KiB |
| 头部与 trailer 累计字段数 | 100 |
| 解码后的累计正文 | 8 MiB |
| chunk 长度行（含 CRLF） | 1 KiB |

在 `start()` 前调用 `set_request_limits()` 调整。请求行、头部和正文超限分别返回
414、431、413；chunk 长度行超限返回 400。解析器拒绝非法请求目标、冲突的
Content-Length，以及同时携带 Content-Length 和 Transfer-Encoding 的请求。
不支持 CONNECT 隧道和 Expect 请求，分别返回 501 和 417。

网络读取、写出和 Keep-Alive 空闲超时分别由 `set_recv_timeout()`、
`set_write_timeout()`、`set_keepalive_timeout()` 设置，单位毫秒，0 关闭。
`set_request_timeout()` 另设从请求首字节到接收完成的总期限，默认 30 秒，0 关闭；
后续半包不会刷新期限，超时直接关闭连接。`TimeoutMiddleware` 检查的是业务处理
耗时，与请求接收期限独立。

## TOML 配置

```toml
[server]
host = "0.0.0.0"
port = 8080
name = "zhttp/1.0"
homepage = ""             # 可选的首页跳转目标
daemon = false

[threads]
count = 4
stack_mode = "independent" # independent | shared

[ssl]
enabled = false
cert_file = "server.crt"
key_file = "server.key"

[logging]
level = "info"

[timeout]
read = 30000
write = 30000
keepalive = 60000
```

通过 `builder.from_config("server.toml")` 加载，再注册路由并调用 `run()`。
后续 Builder 调用覆盖同名配置。缺省字段使用 `ServerConfig` 的默认值，配置在
`build()` 时校验；证书初始化也在构建阶段执行。`daemon = true` 时通过守护进程
运行，服务对象在 fork 后创建。

## 目录与接口

公开头文件位于 include/zhttp/，包含路径继续使用 zhttp/...。私有实现位于 src/：

| 位置 | 职责 |
|---|---|
| src/application/ | HttpApplication、路由与中间件执行 |
| src/message/、content/ | 请求、响应、URI、正文与上传数据 |
| src/router/ | 路由匹配及内部树结构 |
| src/middleware/、rate_limit/、static_files/、session/ | HTTP 策略与具体资源存储 |
| src/protocol/http/、protocol/websocket/ | 协议解析、编码、发送与升级 |
| src/runtime/、config/ | 监听器、Runtime 所有权、日志、配置与进程运行 |
| tests/support/ | 请求构造与网络测试辅助 |

HttpApplication 拥有 Router 与内部 RequestPipeline，只同步构造响应；传输层负责提交和完成通知。
ResponseEncoder 决定 framing，HttpResponseWriter 执行网络写出。连接会话在 HTTP 回调返回后
交接 WebSocket 协议，并继续处理已缓冲的帧。Parser、Pipeline、Writer 和协议实现不安装。

离线执行使用生产入口：

```cpp
zhttp::HttpApplication application;
application.router().get("/health", [](zhttp::HttpContext &context) {
    context.response().text("ok");
});
application.freeze();
auto request = std::make_shared<zhttp::HttpRequest>();
request->set_method(zhttp::HttpMethod::GET);
request->set_path("/health");
zhttp::HttpContext context(request);
application.handle(context);
// 最终消费方在写出成功后报告完成。
context.complete(zhttp::CompletionResult::Completed);
```

build() 为每个服务器创建独立的同步错误日志回调，不注册全局日志器；也可用
server.set_error_handler() 接入应用自己的日志。配置加载、路由和离线处理没有日志初始化副作用。
Daemon 只在显式进程运行入口安装信号处理，返回时恢复原处理器；守护模式应在创建线程前调用。

4.0 的 API 迁移、实际目录树和验证结果见 [重构实施报告](docs/refactoring-report.md)。

## 测试、覆盖率与性能

测试需要 GTest/GMock 和 Brotli decoder (`brotlidec`)。先构建测试二进制，再运行：

```bash
cmake --preset debug
cmake --build --preset debug -j 4
ctest --test-dir build/debug -R '^zhttp\.' --output-on-failure
```

分类过滤使用 `'^zhttp\.unit\.'` 或 `'^zhttp\.integration\.'`。
也可调用 `zhttp_test`、`zhttp_test_unit`、`zhttp_test_integration` 构建目标；这些目标
运行已构建的测试，不负责生成测试二进制。测试覆盖协议模型与解析、路由和中间件、
HTTP/TLS 往返、Keep-Alive、分块与同步流、WebSocket 握手/消息/关闭、配置及进程运行。

覆盖率通过仓库统一脚本生成，需要 `gcovr`：

```bash
coverage/run_coverage.sh
coverage/run_coverage.sh --no-test
```

默认报告在 `coverage/reports/`，zhttp 统计范围为模块内源码与头文件，排除 `tests/`。
`--no-test` 复用已有覆盖率数据并重建报告。历史 summary 不代表当前代码的覆盖率。

性能入口需要 `wrk`，benchmark 不注册到 CTest：

```bash
cmake --preset perf
cmake --build --preset perf --target zhttp_benchmark -j 4
build/perf/zhttp/tests/zhttp_benchmark \
  --mode all --threads 4 --wrk-threads 4 \
  --wrk-connections 256 --wrk-duration 10s --path /
```

`--mode` 支持 `independent`、`shared`、`all`。benchmark fork 本地 HTTP 服务，
输出 wrk 的吞吐、延迟和传输速率。可在同一 benchmark 参数下使用 `perf` 或 `valgrind` 采样；cachegrind 的文本报告
使用 `cg_annotate`。同一机器、构建参数和负载下的吞吐与延迟结果才适合比较。
