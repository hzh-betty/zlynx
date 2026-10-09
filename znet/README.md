# znet 3（C++17）

znet 提供 Linux 协程 TCP/UDP 与 TLS 传输，是 zhttp 的网络底座。
版本 3 是破坏性架构重设计，旧头文件、地址继承树、Acceptor、TcpConnection、
邮箱、网络全局日志和裸 context API 已删除，没有兼容层。

完整评估、P0/P1/P2 问题、依赖图、目录树、删除清单及迁移说明见
[架构重设计](docs/architecture-redesign.md)，验证记录见
[重构验证](docs/refactor-validation.md)。
wrk 的重复对照、持续运行及验证限制也记录在重构验证文档中。

## 最小 echo 服务

```cpp
#include "znet/server/tcp_server.h"
#include <chrono>
#include <iostream>
#include <thread>

int main() {
    zco::Runtime runtime(zco::RuntimeOptions{4});
    auto endpoint = znet::Endpoint::ipv4("0.0.0.0", 18080);
    if (!endpoint) {
        std::cerr << endpoint.error().message() << '\n';
        return 1;
    }
    znet::ServerOptions options;
    options.write_timeout = std::chrono::seconds{30};
    options.idle_timeout = std::chrono::seconds{60};
    options.on_error = [](const znet::Error &error) {
        std::cerr << error.message() << '\n';
    };
    znet::TcpServer server(runtime, std::move(endpoint).value(),
        [](const znet::Connection::ptr &) {
            return znet::SessionCallbacks{
                [](const znet::Connection::ptr &connection,
                   znet::ByteBuffer &input) {
                    auto sent = connection->send(input.view());
                    if (!sent) {
                        // bytes 保留已发送进度；传输失败后连接已关闭。
                        std::cerr << sent.error.message() << '\n';
                    }
                    input.retrieve_all();
                }, {}};
        }, options);
    auto started = server.start();
    if (!started) {
        std::cerr << started.error().message() << '\n';
        return 1;
    }
    while (server.is_running())
        std::this_thread::sleep_for(std::chrono::seconds{1});
    server.stop();
}
```

Runtime 由应用拥有，并最后销毁。TcpServer 可放在栈上或由 unique_ptr 拥有。
多个服务器可以借用同一个 Runtime。start/stop 在控制线程调用；业务回调使用
request_stop 发出停止请求，控制线程的 stop 等待所有接入、握手、消息及关闭回调完成。
析构自动停止；不要求 shared_from_this，也不停止应用的 Runtime。

## 边界与所有权

```text
协议会话 → TcpServer → Connection → ByteStream
                             TCP/TLS 实现 → Socket → zco 就绪等待
                             TLS 实现 → OpenSSL
协议会话 → ByteBuffer（纯字节数据，无 IO 依赖）
```

- Endpoint 是校验过的地址值；支持 IPv4/IPv6/Unix，包括 Linux 抽象 Unix 地址。
  无效 IP、超长路径或截断 sockaddr 返回错误，不回退到 ANY 或截断路径。
- resolve_endpoints 返回地址值列表或解析错误，属于启动配置时的阻塞操作。
- Socket 是 move-only RAII 资源，adopt 消费 fd（失败也会关闭），保持非阻塞与 CLOEXEC。
  bind/listen/选项/端点查询可在普通线程调用；accept/connect/读写必须在 zco 任务内。
  UDP receive_from 按值返回来源、字节数及截断标志；零长度数据报仍是消息。
- ByteStream 只描述传输生命周期和字节 IO；Connection 唯一拥有其实现。
  TCP/TLS/测试假传输使用同一契约，公共头文件不暴露 OpenSSL 类型。
- Connection 不拥有输入/输出队列或协议状态。read 追加到调用者提供的 ByteBuffer；
  send 同步发送并返回真实进度。读与写分别串行化，因此空闲读不会阻塞发送。
  普通线程调用通过显式 executor 提交并等待，协程直接调用，无全局运行时。
- SessionFactory 每连接调用一次，返回消息/关闭回调。协议状态由回调闭包拥有；
  WebSocket 等观察者使用 weak_ptr，不形成连接与业务状态的所有权环。
  不同连接的工厂及 on_error 可能并发调用；同一会话的消息、关闭回调依次执行。
  on_close 在传输关闭后调用，需要保留的端点信息应在会话建立时捕获。
- ByteBuffer 支持连续视图、追加、消费、CRLF 查找、增长与自追加。单个缓冲只由
  一个会话访问；reserve/append 会使借用的指针/视图失效。它可以独立于网络测试。

## 错误与超时

运行时失败返回 Result<T>/Result<void>；传输返回 Transfer：bytes、Error、eof。
进度和失败可以同时存在，不能仅判断 bytes。Error 包含分类、error_code、操作与细节；
校验/运行时/IO/协议/应用错误可区分。必要依赖为空、访问 Result 的错误分支等编程错误
抛出异常。底层不记录日志，服务边界只通过显式 on_error 报告终止错误，应用决定日志策略。
正常 EOF、停止及读取期限到期不报告；握手失败（包括握手超时）会报告。

超时配置使用 chrono::milliseconds，0 表示无限，负数无效。read 接收绝对 zco::Deadline；
set_read_deadline 为后续读取设置更早的请求/关闭截止时间，空 Deadline 清除它。
此设置不打断已经发起的 read，close 则立即撤销 fd 并唤醒所有 IO 等待。
send 的可选 timeout 覆盖连接默认写超时，整个调用的排队和部分写入共用一个预算。
发送在传输阶段失败后关闭连接，避免不完整帧和 TLS 写重试状态污染下一次发送。

read 的 eof 表示对端写方向结束，连接仍允许发送最后的响应；close 才释放 fd。
shutdown 尝试发送 TLS close_notify/SHUT_WR 后关闭，默认无限写超时时关闭预算为 1 秒。
原写完成/高水位回调与 flush_output 已删除：同步发送结果负责确认完成，等待写出本身提供背压。

## TLS

在创建服务器前显式加载凭据：

```cpp
auto credentials = znet::TlsCredentials::load("cert.pem", "key.pem");
if (!credentials)
    throw std::runtime_error(credentials.error().message());
options.tls = std::make_shared<const znet::TlsCredentials>(
    std::move(credentials).value());
options.handshake_timeout = std::chrono::seconds{10};
```

TLS 最低 1.2，加载完整证书链并验证私钥。握手属于传输启动阶段，服务器在握手前登记连接，
因此 stop 能取消无限握手。TLS 在 OpenSSL 调用期间互斥，在 IO 等待期间释放互斥锁。
自定义 Socket BIO 使用 MSG_NOSIGNAL，不修改全进程 SIGPIPE 行为。fd 借用覆盖每次 SSL 调用，
避免并发关闭后使用已复用 fd。SSL 对象在传输析构时释放，凭据可以早于传输销毁。

## 构建、测试与安装

依赖：Linux、C++17、CMake 3.18+、zco 2、OpenSSL 1.1+；测试需要 GTest/GMock、
openssl 命令行工具。网络库不依赖 zlog，OpenSSL 为私有实现依赖。

```bash
cmake --preset debug
cmake --build --preset debug
ctest --test-dir build/debug --output-on-failure
ctest --test-dir build/debug -R '^znet\.' --output-on-failure
cmake --preset release
cmake --build --preset release
cmake --install build/release --prefix /path/to/install
```

安装后：

```cmake
find_package(znet 3 CONFIG REQUIRED)
add_executable(application main.cc)
target_link_libraries(application PRIVATE znet::znet)
```

测试覆盖纯值模型、假传输和真实 TCP/UDP/TLS，包含并发发送、读写并行、部分写入、取消、
超时、回调异常、析构、重启与共享运行时。所有测试通过公开契约访问，无 private-public 宏。
历史 coverage/znet-summary.txt 不能代表此次重写的覆盖率；需要用 coverage/run_coverage.sh 重新生成。

性能程序及脚本沿用入口并已迁移到新 API：

```bash
cmake --preset perf
cmake --build --preset perf --target znet_wrk_benchmark
build/perf/znet/tests/znet_wrk_benchmark --threads 4 --wrk-duration 5s
BUILD_DIR=build/perf znet/tests/benchmark/znet_wrk_perf.sh baseline
```
