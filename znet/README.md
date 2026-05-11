# znet

`znet` 是 zlynx 的 TCP 网络模块，基于 `zco` 协程运行时提供 TCP server、连接对象、
字节缓冲、地址抽象、socket 封装和 TLS 支持。它是 `zhttp` 的网络底座，也可以被
业务代码直接用来构建自定义协议。

## 快速开始

下面是一个最小 echo server。`TcpServer::start()` 会初始化 `zco` runtime 并启动
accept loop，业务通过连接和消息回调处理。

```cpp
#include "znet/address.h"
#include "znet/tcp_server.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

int main() {
    auto addr = std::make_shared<znet::IPv4Address>("0.0.0.0", 18080);
    auto server = std::make_shared<znet::TcpServer>(addr);

    server->set_thread_count(4);
    server->set_read_timeout(30000);
    server->set_write_timeout(30000);
    server->set_keepalive_timeout(60000);

    server->set_on_connection([](const znet::TcpConnection::ptr &conn) {
        std::cout << "connected fd=" << conn->fd() << std::endl;
    });

    server->set_on_message([](const znet::TcpConnection::ptr &conn,
                              znet::Buffer &buffer) {
        std::string payload = buffer.retrieve_all_as_string();
        conn->send(payload.data(), payload.size());
    });

    server->set_on_close([](const znet::TcpConnection::ptr &conn) {
        std::cout << "closed fd=" << conn->fd() << std::endl;
    });

    if (!server->start()) {
        return 1;
    }

    while (server->is_running()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return 0;
}
```

安装后消费：

```cmake
cmake_minimum_required(VERSION 3.18)
project(znet_demo LANGUAGES CXX)

find_package(znet CONFIG REQUIRED)

add_executable(znet_demo main.cc)
target_link_libraries(znet_demo PRIVATE znet::znet)
```

源码树内开发可以直接链接 `znet` target。

## 项目架构

`znet` 处在协程运行时和 HTTP 层之间：

```text
zhttp
  -> znet
      -> zco      协程调度、hook、同步原语
      -> OpenSSL  TLS context/channel
```

核心目录：

```text
znet/
  include/znet/             公共 API：TcpServer、TcpConnection、Buffer、Address 等
  include/znet/internal/    基础 noncopyable
  src/                      网络模块实现
  tests/unit/               单元测试
  tests/integration/        socket/TCP/TLS 集成测试
  tests/benchmark/          wrk benchmark 和 perf/valgrind 脚本
```

主要组件：

- `Address`、`IPv4Address`、`IPv6Address`、`UnixAddress`：网络地址抽象和 DNS lookup。
- `Socket`：socket fd 生命周期、bind/listen/accept/connect、选项设置和读写封装。
- `Acceptor`：监听 socket 与 accept loop。
- `Buffer`：网络 I/O 字节缓冲，支持 prepend 空间、append、retrieve 和 socket 读写。
- `TcpConnection`：单连接状态机、输入/输出缓冲、send/flush/shutdown/close、TLS channel。
- `TcpServer`：连接表、回调注册、线程数、超时、TLS、连接分发和 graceful stop。
- `TlsContext`：OpenSSL server context 初始化、证书加载和握手支持。
- `znet_logger`：模块日志初始化与日志宏。

## 依赖

基础构建依赖：

- CMake 3.18+
- C++14 编译器，仓库 preset 默认使用 `clang++`
- Ninja，使用 preset 时需要
- Linux/POSIX，当前 CMake 明确拒绝非 UNIX 或 Apple 平台
- `zco`
- OpenSSL

测试和分析额外依赖：

- GTest / GMock
- Threads
- `gcovr`
- `wrk`
- `perf`、`valgrind`、`cg_annotate`

## 编译

```bash
cmake --preset debug
cmake --build --preset debug
```

发布构建：

```bash
cmake --preset release
cmake --build --preset release
```

构建 wrk benchmark：

```bash
cmake --preset perf
cmake --build --preset perf --target znet_wrk_benchmark
```

手动配置：

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

安装后导出 `znet::znet`，并通过包配置转发 `zco` 和 `OpenSSL` 依赖。

## 测试

运行全部 znet 测试：

```bash
cmake --build --preset debug --target znet_test
```

只跑单元测试：

```bash
cmake --build --preset debug --target znet_test_unit
```

只跑集成测试：

```bash
cmake --build --preset debug --target znet_test_integration
```

直接使用 CTest：

```bash
ctest --test-dir build/debug -R '^znet\.' --output-on-failure
ctest --test-dir build/debug -R '^znet\.unit\.' --output-on-failure
ctest --test-dir build/debug -R '^znet\.integration\.' --output-on-failure
```

当前测试覆盖的主要行为：

- `Address` IPv4/IPv6/Unix 地址格式化、端口、lookup
- `Buffer` append/retrieve、空间扩展、CRLF 查找、socket 读写
- `Socket` 创建、bind/listen/connect、socket option、fd 生命周期
- `Acceptor` accept 回调与关闭路径
- `TcpConnection` 状态机、send/flush、关闭、上下文和高水位回调
- `TcpServer` start/stop、连接回调、消息回调、串行资源锁
- `TlsContext` 证书加载、OpenSSL context、TLS round trip
- 模块日志

## 覆盖率

统一脚本：

```bash
coverage/run_coverage.sh
```

`coverage/znet-summary.txt` 中记录的当前 znet 覆盖率：

| 指标 | 覆盖率 |
|---|---:|
| Lines | 92.2% (1082 / 1173) |
| Functions | 97.3% (145 / 149) |
| Branches | 80.5% (729 / 906) |
| Decisions | 87.3% (165 / 189) |

覆盖率报告只统计 `znet/src`。脚本中对 znet 额外过滤了日志宏展开分支和部分
OpenSSL 状态机错误码分支，避免第三方状态分发污染模块指标。

## 性能

`znet_wrk_benchmark` 会 fork 一个本地 TCP/HTTP-like server，并使用 `wrk` 压测
`http://127.0.0.1:<port>/<path>`，输出 `Requests/sec`、`Latency`、
`Transfer/sec` 摘要。

```bash
cmake --preset perf
cmake --build --preset perf --target znet_wrk_benchmark

build/perf/znet/tests/znet_wrk_benchmark \
  --threads 4 \
  --wrk-threads 4 \
  --wrk-connections 256 \
  --wrk-duration 5s \
  --path /
```

脚本入口：

```bash
BUILD_DIR=build/perf znet/tests/benchmark/znet_wrk_perf.sh baseline
BUILD_DIR=build/perf znet/tests/benchmark/znet_wrk_perf.sh perf
BUILD_DIR=build/perf znet/tests/benchmark/znet_wrk_perf.sh valgrind
```

benchmark 支持的常用参数：

```bash
--port 18080
--threads 4
--wrk-bin wrk
--wrk-threads 4
--wrk-connections 256
--wrk-duration 5s
--warmup-ms 300
--path /
--scale-pct 100
--server-ready-timeout-ms 3000
--shutdown-timeout-ms 5000
--wrk-arg <arg>
```

性能测试结果与 CPU、内核、OpenSSL、wrk 参数、fd 限制、线程数和系统负载相关。
比较优化前后结果时应固定机器、构建类型和压测参数。

## 支持功能

- IPv4、IPv6、Unix Domain Socket 地址抽象
- DNS/host lookup
- Socket fd 生命周期和常用 socket option
- Acceptor 监听与连接接受
- `TcpServer` 多线程/多协程连接分发
- 连接建立、消息到达、关闭、写完成、高水位回调
- 每连接输入/输出缓冲
- 连接级 read/write/keepalive timeout
- TLS server context、证书加载和握手
- Buffer prepend、append、retrieve、自动扩容和 socket I/O
- 和 `zco` runtime 集成的协程化网络 I/O

