# zhttp

`zhttp` 是 zlynx 的 HTTP/WebSocket 服务模块，构建在 `znet` TCP 网络层和
`zlog` 日志模块之上，面向需要在 C++14 中快速嵌入 HTTP API、静态文件服务、
中间件链和 WebSocket 通道的开发者。

## 快速开始

最小服务只需要包含统一头文件 `zhttp/zhttp.h`，使用 `HttpServerBuilder`
配置监听地址、线程数和路由，然后调用 `run()` 进入阻塞运行。

```cpp
#include "zhttp/zhttp.h"

int main() {
    zhttp::HttpServerBuilder builder;

    builder.listen("0.0.0.0", 8080)
        .threads(4)
        .server_name("demo-zhttp")
        .get("/health",
             [](const zhttp::HttpRequest::ptr &, zhttp::HttpResponse &resp) {
                 resp.status(zhttp::HttpStatus::OK).json(R"({"ok":true})");
             })
        .get("/users/:id",
             [](const zhttp::HttpRequest::ptr &req, zhttp::HttpResponse &resp) {
                 resp.text("user=" + req->path_param("id"));
             })
        .websocket(
            "/ws",
            zhttp::WebSocketCallbacks{
                {},
                [](const zhttp::WebSocketConnection::ptr &conn,
                   std::string &&message, zhttp::WebSocketMessageType type) {
                    if (type == zhttp::WebSocketMessageType::kText) {
                        conn->send_text(message);
                    }
                },
                {},
                {}});

    builder.run();
    return 0;
}
```

如果在源码树内开发，可以直接链接 `zhttp` target；安装后消费则使用
`zhttp::zhttp`。

```cmake
cmake_minimum_required(VERSION 3.18)
project(zhttp_demo LANGUAGES CXX)

find_package(zhttp CONFIG REQUIRED)

add_executable(zhttp_demo main.cc)
target_link_libraries(zhttp_demo PRIVATE zhttp::zhttp)
```

## 项目架构

`zhttp` 位于 zlynx 依赖链的最上层：

```text
zhttp
  -> znet   TCP server、连接、Buffer、TLS context
  -> zlog   日志输出与日志级别
  -> 第三方库: OpenSSL、fmt、ZLIB、Brotli、nlohmann_json、toml.hpp
```

核心目录：

```text
zhttp/
  include/zhttp/              公共头文件与统一入口 zhttp.h
  include/zhttp/internal/     HTTP parser、radix tree、range/http 工具
  include/zhttp/mid/          内置中间件
  src/                        模块实现
  src/mid/                    内置中间件实现
  tests/unit/                 单元测试
  tests/integration/          HTTP/WebSocket 端到端测试
  tests/benchmark/            wrk/perf/valgrind 性能入口
```

主要组件：

- `HttpServerBuilder`：链式收集监听地址、线程数、HTTPS、超时、路由、中间件、
  日志和守护进程配置，最终 `build()` 或 `run()`。
- `HttpServer`：复用 `znet::TcpServer` 管理连接，在消息回调中完成 HTTP 解析、
  路由分发、响应序列化、chunked 流式响应和 WebSocket 升级。
- `Router`：提供静态路由哈希查找、动态路由 radix tree、正则路由前缀分桶；
  支持全局中间件、路由中间件和前缀组中间件。
- `HttpRequest` / `HttpResponse`：封装请求字段、路径/查询/Cookie 参数、JSON、
  form、multipart、响应头、Cookie、重定向、文本/HTML/JSON、同步和异步流式响应。
- `WebSocketSession` / `WebSocketConnection`：处理握手、子协议协商、帧解析、
  text/binary/ping/pong/close 发送和生命周期回调。
- `mid::*Middleware`：内置横切能力，包括鉴权、角色授权、CORS、压缩、错误处理、
  限流、请求体限制、安全响应头、Session、静态文件和超时。
- `ServerConfig`：支持从 TOML 加载运行配置，并验证端口、线程、HTTPS 重定向和
  homepage 等配置组合。

## 依赖

基础构建依赖：

- CMake 3.18+
- C++14 编译器，仓库 preset 默认使用 `clang++`
- Ninja，使用 preset 时需要
- POSIX/Linux 网络与进程接口
- `znet`、`zlog`
- OpenSSL
- fmt
- ZLIB
- Brotli encoder library，CMake 查找名为 `brotlienc`
- nlohmann_json
- 提供 `<toml.hpp>` 的 TOML 头文件实现

测试和分析额外依赖：

- GTest / GMock
- Threads
- Brotli decoder library，测试查找名为 `brotlidec`
- `gcovr`，生成覆盖率报告
- `wrk`，运行 HTTP benchmark
- `perf`、`valgrind`、`cg_annotate`，可选性能分析工具

## 编译

推荐使用仓库根目录的 CMake presets。

```bash
cmake --preset debug
cmake --build --preset debug
```

发布构建：

```bash
cmake --preset release
cmake --build --preset release
```

只需要性能测试二进制时：

```bash
cmake --preset perf
cmake --build --preset perf --target zhttp_benchmark
```

也可以手动配置：

```bash
cmake -S . -B build/debug -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON
cmake --build build/debug -j
```

安装：

```bash
cmake --build --preset release --target install
```

安装后会导出 `zhttp::zhttp`，包配置文件位于标准
`<prefix>/lib/cmake/zhttp` 路径下。`zhttp` 的 public headers 会向消费者传播
`znet`、`zlog` 和 `nlohmann_json` 用法需求。

## 测试

启用 `BUILD_TESTING=ON` 后，`zhttp/tests/CMakeLists.txt` 会注册单元测试和集成测试。
benchmark 目标不会进入 CTest。

运行全部 zhttp 测试：

```bash
cmake --build --preset debug --target zhttp_test
```

只跑单元测试：

```bash
cmake --build --preset debug --target zhttp_test_unit
```

只跑集成测试：

```bash
cmake --build --preset debug --target zhttp_test_integration
```

也可以直接用 CTest 过滤：

```bash
ctest --test-dir build/debug -R '^zhttp\.' --output-on-failure
ctest --test-dir build/debug -R '^zhttp\.unit\.' --output-on-failure
ctest --test-dir build/debug -R '^zhttp\.integration\.' --output-on-failure
```

当前测试覆盖的主要行为：

- HTTP request/response/common/parser/utils
- 路由静态匹配、动态参数、正则前缀分桶、路由组中间件
- HTTP server 上下文、keep-alive、split packet、chunked request/response
- HTTPS round trip 和 HTTP -> HTTPS 重定向
- WebSocket 握手、子协议协商、帧解析、echo、ping/pong/close
- JSON、form-urlencoded、multipart/form-data
- 静态文件、ETag、If-Modified-Since、Range、预压缩资源、内存缓存
- CORS、鉴权、角色授权、压缩、错误处理、限流、请求体限制、安全头、Session、超时
- TOML 配置、守护进程、日志

## 覆盖率

仓库提供统一脚本 `coverage/run_coverage.sh`。脚本会配置覆盖率构建、运行测试，并
按模块输出 summary、branch 报告和 HTML 明细。

```bash
coverage/run_coverage.sh
```

只生成报告、不重新跑测试：

```bash
coverage/run_coverage.sh --no-test
```

指定目录：

```bash
coverage/run_coverage.sh \
  --build-dir build-cov-all \
  --report-dir coverage/reports
```

`coverage/zhttp-summary.txt` 中记录的当前 zhttp 覆盖率：

| 指标 | 覆盖率 |
|---|---:|
| Lines | 94.8% (4026 / 4245) |
| Functions | 98.3% (409 / 416) |
| Branches | 85.1% (3721 / 4372) |
| Decisions | 84.3% (1242 / 1473) |

覆盖率报告只统计 `zhttp/src`，不把 benchmark 和第三方依赖纳入统计。

## 性能

`zhttp_benchmark` 会 fork 一个本地 HTTP server，再用 `wrk` 对
`http://127.0.0.1:<port>/<path>` 施压。它支持比较协程独立栈和共享栈两种模式，
并输出 `Requests/sec`、`Latency` 和 `Transfer/sec` 摘要。

构建 benchmark：

```bash
cmake --preset perf
cmake --build --preset perf --target zhttp_benchmark
```

直接运行：

```bash
build/perf/zhttp/tests/zhttp_benchmark \
  --mode all \
  --threads 4 \
  --wrk-threads 4 \
  --wrk-connections 256 \
  --wrk-duration 10s \
  --path /
```

脚本入口：

```bash
zhttp/tests/benchmark/zhttp_wrk_perf.sh baseline
zhttp/tests/benchmark/zhttp_wrk_perf.sh perf
zhttp/tests/benchmark/zhttp_wrk_perf.sh valgrind
```

常用环境变量：

```bash
BUILD_DIR=build/perf
RESULT_ROOT=zhttp/tests/benchmark/perf_results
ZHTTP_SERVER_THREADS=4
ZHTTP_WRK_THREADS=4
ZHTTP_WRK_CONNECTIONS=256
ZHTTP_WRK_DURATION=10s
ZHTTP_BENCH_MODE=all        # independent | shared | all
ZHTTP_BENCH_PATH=/
```

性能测试建议使用 `RelWithDebInfo`，保留优化和调试符号；preset `perf` 会额外保留
frame pointer，便于 `perf`、火焰图和 cachegrind 还原调用栈。吞吐和延迟结果与
CPU、内核、OpenSSL/ZLIB/Brotli 版本、wrk 参数、线程绑定和系统限制有关，应在同一
机器、同一构建参数下比较。

## 支持功能

- HTTP/1.x 请求解析和响应序列化
- GET、POST、PUT、DELETE 路由注册
- 静态路由、`:param` 动态路由、`*catch-all` 路由和正则路由
- 全局、路由级和前缀组中间件
- 自定义 404 和异常处理
- JSON、HTML、文本、重定向响应
- Cookie 和 Set-Cookie，支持常用 Cookie 属性
- 查询参数、路径参数、Cookie、JSON body、form-urlencoded body
- multipart/form-data 表单和文件上传解析
- chunked request body、显式 chunked response、同步流和异步推送流
- Keep-Alive、读/写/空闲超时
- HTTPS 和 HTTP 到 HTTPS 308 重定向
- WebSocket 升级、子协议协商、文本/二进制消息、ping/pong/close
- 静态文件服务，支持路径清理、隐式 index、ETag、If-Modified-Since、Range、
  预压缩 br/gzip 选择和小文件内存缓存
- gzip/br 响应压缩
- CORS、安全响应头、请求体大小限制、错误中间件
- Token bucket、fixed window、sliding window 限流
- Session 注入和 Cookie 写回
- TOML 配置加载
- 前台/守护进程运行
- 可切换协程栈模式：`INDEPENDENT` 和 `SHARED`

## TOML 配置示例

```toml
[server]
host = "0.0.0.0"
port = 8080
name = "zhttp/1.0"
homepage = "/dashboard"
daemon = false

[threads]
count = 4
stack_mode = "independent" # independent | shared

[ssl]
enabled = false
cert_file = "server.crt"
key_file = "server.key"
force_http_to_https = false
redirect_http_port = 80

[logging]
level = "info"

[timeout]
read = 30000
write = 30000
keepalive = 60000
```

代码中加载：

```cpp
zhttp::HttpServerBuilder builder;
builder.from_config("server.toml")
       .get("/ping", [](const zhttp::HttpRequest::ptr &,
                        zhttp::HttpResponse &resp) {
           resp.text("pong");
       })
       .run();
```

