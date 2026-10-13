# znet

znet 是基于 zco 的 Linux/C++17 网络库，提供 TCP 服务、UDP socket、TLS 传输、
连接和字节缓冲。依赖 zco、OpenSSL 1.1+ 和 Threads。

## 最小 echo 服务

```cpp
#include "znet/server/tcp_server.h"
#include <iostream>
#include <utility>

int main() {
    zco::RuntimeOptions runtime_options;
    runtime_options.worker_count = 2;
    zco::Runtime runtime(runtime_options);
    auto endpoint = znet::Endpoint::ipv4("127.0.0.1", 18080);
    if (!endpoint) {
        std::cerr << endpoint.error().message() << '\n';
        return 1;
    }

    znet::TcpServer server(runtime, std::move(endpoint).value(),
        [](const znet::Connection::ptr &) {
            znet::SessionCallbacks callbacks;
            callbacks.on_message = [](const znet::Connection::ptr &connection,
                                      znet::ByteBuffer &input) {
                auto sent = connection->send(input.view());
                if (!sent)
                    std::cerr << sent.error.message() << '\n';
                input.retrieve_all();
            };
            return callbacks;
        });
    auto started = server.start();
    if (!started) {
        std::cerr << started.error().message() << '\n';
        return 1;
    }
    std::cout << "Listening on 127.0.0.1:18080; press Enter to stop.\n";
    std::cin.get();
    server.stop();
    return 0;
}
```

Runtime 由应用拥有，必须比服务器活得更久。`start()` / `stop()` 在控制线程调用，
业务回调可调用 `request_stop()`，由控制线程随后执行 `stop()` 等待所有会话退出。
服务器析构自动停止，自身不停止借用的 Runtime。

## 组件与契约

| 组件 | 职责 |
| --- | --- |
| `Endpoint` | IPv4、IPv6、Unix 地址值与校验；解析主机名是阻塞操作 |
| `Socket` | 非阻塞 RAII socket、连接、接入、读写及 UDP 数据报 |
| `ByteStream` | TCP/TLS 共用的字节传输与生命周期接口 |
| `Connection` | 独占传输；读与写分别串行化，返回真实传输进度 |
| `TcpServer` | 接入连接，为每个连接创建并运行会话回调 |
| `ByteBuffer` | 字节追加、视图与消费，无 I/O 依赖 |

Socket 的接入、连接及传输操作必须在 zco 任务中使用。Connection 的普通线程
调用通过注入的 Executor 提交并等待，协程调用直接执行。

协议状态和输入缓冲由会话持有。`SessionFactory` 每连接调用一次；同一会话的
消息和关闭回调依次运行，不同连接可并发执行。`ByteBuffer` 增长或消费后，
调用方应重新获取视图，不能长期保留借用的指针。

`send()` 成功表示全部字节已发送，失败仍保留已发送字节数；传输阶段失败会关闭
连接。读取 EOF 后仍可发送最后的响应，`close()` 释放传输并中断等待。
传输结果同时包含进度、错误和 EOF，不能仅凭字节数判断成功。

超时使用 `std::chrono::milliseconds`，0 表示无限，负数无效；一次发送的排队及
部分写入共用一个时间预算。底层不记录日志，服务边界可通过 `ServerOptions::on_error`
接收终止错误，由应用决定如何报告。

## TLS

在启动服务器前加载证书，并通过 `ServerOptions` 配置：

```cpp
auto credentials = znet::TlsCredentials::load("cert.pem", "key.pem");
if (!credentials)
    throw std::runtime_error(credentials.error().message());
znet::ServerOptions server_options;
server_options.tls = std::make_shared<const znet::TlsCredentials>(
    std::move(credentials).value());
server_options.handshake_timeout = std::chrono::seconds(10);
```

将 `server_options` 作为 TcpServer 构造的第四个参数。TLS 握手属于传输启动阶段，
服务器停止时可取消正在等待的握手。公共传输接口不暴露 OpenSSL 类型。

如需在无 OpenSSL 环境下构建，可关闭 TLS 传输实现：

```bash
cmake -S znet -B build/znet-notls -DCMAKE_BUILD_TYPE=Debug -DZNET_ENABLE_TLS=OFF
```

关闭后 `TlsCredentials::load()` 会返回 `operation_not_supported`，TCP 明文传输、
UDP、地址解析和缓冲组件仍可正常构建与使用。

## 构建、测试与安装

整体构建方式见[根 README](../README.md)。独立构建需先安装 zco，以下命令从
仓库根目录执行，需要 CMake 3.18+；默认测试还需要 GTest 和 `openssl` 命令：

```bash
cmake -S znet -B build/znet -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH=/path/to/prefix
cmake --build build/znet -j 2
(cd build/znet && ctest --output-on-failure)
cmake --install build/znet --prefix /path/to/prefix
```

仅构建库可加 `-DBUILD_TESTING=OFF`，共享库可加 `-DBUILD_SHARED_LIBS=ON`。
整体构建后用 `ctest -R '^znet_'` 筛选缓冲、地址、socket、连接、TCP 服务和 TLS 测试。
关闭 TLS 时会自动跳过 `tls_stream` 集成测试目标。

消费者通过同一安装前缀查找包：

```cmake
find_package(znet 1 CONFIG REQUIRED)
add_executable(app main.cc)
target_link_libraries(app PRIVATE znet::znet)
```

安装包会查找 zco、OpenSSL 和 Threads。

## 性能与源码

```bash
cmake --preset relwithdebinfo -DBUILD_BENCHMARKS=ON
cmake --build --preset relwithdebinfo --target znet_wrk_benchmark -j 2
```

以上命令从仓库根目录执行，性能驱动位于 [tests/benchmark](tests/benchmark)。
覆盖率入口见[根 README](../README.md)。

公共接口位于 [include/znet](include/znet)，服务与会话实现位于
[src/server](src/server)，TCP/TLS/socket 实现位于 [src/transport](src/transport)。
