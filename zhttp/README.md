# zhttp

zhttp 是基于 znet、zco 和 zlog 的 Linux/C++17 HTTP/WebSocket 框架，提供请求与
响应、路由、中间件、HTTP/HTTPS 服务及 WebSocket 会话。

## 最小 HTTP 服务

```cpp
#include "zhttp/zhttp.h"
#include <iostream>

int main() {
    zhttp::HttpServerBuilder builder;
    builder.listen("127.0.0.1", 18080)
        .threads(2)
        .get("/", [](zhttp::HttpContext &context) {
            context.response().text("hello zlynx\n");
        });
    auto server = builder.build();
    auto started = server->start();
    if (!started) {
        std::cerr << started.error().message() << '\n';
        return 1;
    }
    std::cout << "Listening on 127.0.0.1:18080; press Enter to stop.\n";
    std::cin.get();
    server->stop();
    return 0;
}
```

编译运行后，在另一个终端访问：

```bash
curl http://127.0.0.1:18080/
```

示例监听本地地址，按 Enter 停止。`HttpServerBuilder` 负责配置和组装服务，
`build()` 返回服务器，`start()` / `stop()` 由控制线程管理生命周期。
需要共享运行时的应用也可显式向 HttpServer 注入 `zco::Runtime`。

## 请求处理与功能

```text
TCP/TLS 连接 → HTTP 解析 → HttpContext → 中间件 → 路由 → 响应写出
                                      └→ WebSocket 升级与帧处理
```

`HttpApplication` 提供不启动监听器的同步路由和中间件入口；注册完成后调用
`freeze()`，冻结后可并发处理不同请求。业务处理器中的共享状态仍由应用同步。
`HttpContext` 持有只读请求、可构造的响应和本次请求的派生数据。

- 路由支持静态、动态及正则路径，处理器接收 `HttpContext &`。
- 中间件支持错误处理、认证、CORS、安全响应头、限流、Session、请求体处理、
  静态文件、压缩和处理耗时检查。
- 内容接口支持 JSON、表单、multipart；响应支持内存、文件及流式正文。
- 静态文件支持范围请求；压缩支持 gzip 与 Brotli。
- 服务支持 TLS、Keep-Alive、请求限制、WebSocket 升级和消息回调。
- `ServerConfig` 和 Builder 支持 TOML 配置及运行时参数。

请求处理器同步构造响应，由协议层负责写出和完成通知。不得保留上下文引用用于
请求结束后的操作；响应提交后不能继续修改。`TimeoutMiddleware` 检查处理耗时，
不会抢占正在执行的业务函数；I/O 超时和请求限制由服务器配置分别管理。

HTTPS 可通过 Builder 的 `enable_https(cert_file, key_file)` 配置证书，
WebSocket 可通过 `websocket(path, callbacks, options)` 注册。
具体参数和回调契约见 [HttpServerBuilder](include/zhttp/http_server_builder.h) 与
[WebSocketHandler](include/zhttp/websocket/websocket_handler.h)。

## 依赖

- Linux、C++17、CMake 3.18+。
- 已构建或安装的 zco、znet、zlog。
- fmt、nlohmann_json、toml11、OpenSSL 1.1+、zlib、Threads。
- pkg-config 和 Brotli 编码库（`libbrotlienc`）。
- 开启测试时还需要 GTest、Brotli 解码库（`libbrotlidec`）。
- wrk 为可选压测工具。

## 构建、测试与安装

整体构建方式见[根 README](../README.md)。独立构建需先安装 zco、znet、zlog，
以下命令从仓库根目录执行：

```bash
cmake -S zhttp -B build/zhttp -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH=/path/to/prefix
cmake --build build/zhttp -j 2
(cd build/zhttp && ctest --output-on-failure)
cmake --install build/zhttp --prefix /path/to/prefix
```

默认开启测试；仅构建库可加 `-DBUILD_TESTING=OFF`，共享库可加
`-DBUILD_SHARED_LIBS=ON`。整体构建后用 `ctest -R '^zhttp_'` 筛选解析、路由、
中间件、配置、HTTP 服务及 WebSocket 测试。

消费者通过 `-DCMAKE_PREFIX_PATH=/path/to/prefix` 查找安装包：

```cmake
find_package(zhttp 1 CONFIG REQUIRED)
add_executable(app main.cc)
target_link_libraries(app PRIVATE zhttp::zhttp)
```

安装包会查找上述依赖，并传递公共编译和链接要求。

## 性能与源码

```bash
cmake --preset relwithdebinfo -DBUILD_BENCHMARKS=ON
cmake --build --preset relwithdebinfo \
  --target zhttp_benchmark zhttp_core_benchmark -j 2
```

以上命令从仓库根目录执行。`zhttp_benchmark` 借助 wrk 压测服务，
`zhttp_core_benchmark` 测量核心请求处理路径。源码和脚本位于
[tests/benchmark](tests/benchmark)，覆盖率入口见[根 README](../README.md)。

公开接口位于 [include/zhttp](include/zhttp)。实现按
[application](src/application)、[protocol](src/protocol)、[message](src/message)、
[router](src/router)、[middleware](src/middleware)、[runtime](src/runtime)
等目录划分，分别负责请求管线、协议、消息、路由、中间件和服务运行。
