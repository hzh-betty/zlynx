# znet 3 重构验证记录

架构验收日期：2026-10-08；2026-10-09 补充 wrk 压测和 IO 调度调整后的回归。
环境：Linux x86_64、Clang 21.1.8、Ninja、OpenSSL 3。
全部项目编译命令使用 `-std=c++17`，禁用 C++ 扩展；导出目标声明 `cxx_std_17`。
本记录对应 [架构重设计](architecture-redesign.md) 中已经实施的最终结构。

## 构建与执行结果

| 检查 | 配置与结果 |
|---|---|
| 修改前基线 | Debug、shared、allocator override ON；74/74 CTest 通过 |
| 最终全项目 Debug | shared、allocator override ON；构建成功，72/72 CTest 通过 |
| 最终全项目 Release | shared、测试 OFF；全部库构建成功 |
| 最终全项目静态 Release | static、系统 allocator、测试 ON；构建成功，71/71 CTest 通过 |
| znet 契约测试 | 8 个 CTest 可执行文件，共 42 个 GTest 用例；全部通过 |
| 安装后共享库消费 | `find_package(znet 3)` / `find_package(zhttp 3)`；独立程序编译、TCP echo 往返及 HTTP 启停通过 |
| 安装后静态库消费 | 同一独立程序只链接导出目标；编译、链接和运行通过，OpenSSL 私有链接依赖正确传递 |
| 公共头文件 | 8 个头文件分别生成独立翻译单元，以 C++17、`-Wall -Wextra -Wpedantic -Werror` 编译通过 |
| 独立 znet 构建 | 从安装前缀获取 zco，Release、测试 OFF、perf ON、禁用 GTest 查找；库及 benchmark 构建成功 |
| benchmark 接口迁移 | 项目内和独立构建通过，`--help` 正常执行 |
| 静态分析 | 8 个 znet 实现及迁移后的 http_server.cc；项目 clang-tidy 的诊断/分析检查无告警、无错误 |
| 差异与依赖 | `git diff --check` 通过；模块/公共头文件 include 图无环；旧 API 引用、private-public 宏、遗留 TODO 均未发现 |
| 10-09 IO 调度回归 | 全项目 Debug 72/72；全项目共享 Release 构建、安装后 echo/HTTP 消费、静态 Release znet 8/8、clang-tidy 通过 |
| wrk 对照与持续运行 | 两轮 5 场景，82 次运行、73,365,384 次请求，无 wrk 错误，全部正常退出；两次 60 秒运行采样 RSS 均稳定 |
| 响应内容与脚本 | 校验 610,485 个响应，状态码/内容错误 0；脚本 LF 修复后通过 Bash 语法检查和真实执行 |

74→72 是旧 znet 的 10 个测试目标重写为 8 个目标，移除了 actor/acceptor/logger
等已经删除的实现专属测试；核心传输契约由新测试覆盖。静态配置的 71 个目标与共享配置
相差 `zmalloc.unit.zmalloc_allocator_override_shared_test`，没有少运行 znet 或 zhttp 测试。

外部消费测试只使用安装头文件和 CMake 导出目标。消费者先声明 C++11，链接模块后由
`cxx_std_17` 自动提升到 C++17；没有补仓库 include 路径或手工追加 OpenSSL 链接库。
共享库运行时设置安装前缀的动态库搜索路径，包括 zco 所采用的分配器库。

## 关键行为证据

| 边界 | 测试内容 |
|---|---|
| Error / Result | move-only 返回值、错误分类和上下文、错误分支访问、失败时保留传输进度 |
| Endpoint | 非法 IP、原生地址截断、值所有权、IPv4/IPv6、Unix 精确长度/抽象命名空间、解析错误 |
| ByteBuffer | 空范围、CRLF、压缩/增长、自追加、空指针与提交容量校验 |
| Connection 假传输 | 多次部分写入共用一个截止时间、部分失败后关闭、零进展终止、读追加、应用期限、异常释放锁、EOF 后写入 |
| Socket | 唯一 fd 移动/收养、失败收养释放 fd、非阻塞/CLOEXEC、TCP 双 IP 家族、UDP 来源/空消息/截断、Unix 流/数据报 |
| 未命名 Unix 发送方 | 最小系统复现确认来源地址可以缺失；接收空/非空消息仍保留数据并返回未命名端点 |
| 并发与取消 | 同一 worker 的挂起读不阻塞发送、多普通线程发送保持各消息连续、close 唤醒无限等待 |
| 预算与背压 | TCP/TLS 慢接收端造成部分写入，整体超时与已发送进度正确；应用读期限不被后续读取延长 |
| TcpServer 生命周期 | 栈所有权、共享 Runtime、重启/重复启停、关闭回调后 stop 才返回、回调请求停止、普通及回调内析构 |
| 错误处理 | 回调异常只报告一次并释放会话状态；监听失败和 Runtime 失效回滚；配置校验与多线程启停 |
| TLS | 无效/不匹配凭据、凭据早于传输销毁、真实握手与往返、EOF/close_notify、读写并行、握手/写超时 |
| TLS 停止 | 不发送 ClientHello 的客户端触发无限握手；stop 可以取消，业务工厂未被误调用 |
| 上层协议 | 保留完整 zhttp 回归：HTTP/HTTPS、请求分片/期限、响应流/文件、流水线、WebSocket 升级/帧/关闭 |

纯缓冲和值模型测试不创建网络运行时。Connection 单元测试注入脚本化 ByteStream，
只在验证线程到协程的执行边界时使用 Runtime；不需要真实 Socket、证书或服务监听。
网络集成测试使用临时端口、Unix 抽象地址、RAII fd 和临时证书，按公开 API 检查行为。

## 复核结果

生产代码统计范围为 `znet/include/**/*.h` 和 `znet/src/**/*.cc`，包含空行和注释：

| 指标 | 原架构 | 最终架构 |
|---|---:|---:|
| 生产文件 | 20 | 16 |
| 生产代码行 | 3,962 | 1,864 |
| 地址多态类型 | 5 | 1 个 Endpoint 值类型 |
| 连接调度 actor / 邮箱 | 1 | 0 |
| 网络全局日志依赖 | zlog | 无 |
| 类型擦除连接业务状态 | void* | 无，状态由会话闭包持有 |

最终跨模块 include 依赖：`znet → zco`，`zhttp → znet/zco/zlog`；
zmalloc、zlog、zco 不反向包含网络模块。ByteStream 接口不提及具体 Socket 类型，
基础设施提供 TCP 创建函数和 TLS 实现；ByteBuffer 不包含网络或协程头文件。

Socket/ByteStream 唯一拥有资源；服务器借用 Runtime，按值拥有会话登记项；
连接句柄和单次服务器运行实例仅在任务具有独立寿命时共享。没有连接反向拥有服务器的环，
没有协议 state/current 的共享环。一个连接只有一份生命周期状态，一份应用读截止时间；
输入缓冲归会话，不保留隐藏输出状态或 fd/地址可变副本。

已删除旧生产文件和原 include 入口，迁移全部 zhttp/benchmark 调用方，未保留适配层。
针对协议切换必须保留的 pending 状态，仍在当前 HTTP 回调返回后才移交所有权；
旧回调模型的驱动/关闭重入标志已经删除。仅保留真实的传输多实现边界及资源封装。

## 复现入口与未验证范围

```bash
cmake --preset debug
cmake --build --preset debug -j4
ctest --test-dir build/debug --output-on-failure -j4

cmake --preset release
cmake --build --preset release -j4

cmake -S . -B /tmp/zlynx-static -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=ON \
  -DZLYNX_USE_ZMALLOC_OVERRIDE=OFF -DZLYNX_BUILD_PERF_TESTS=OFF
cmake --build /tmp/zlynx-static -j4
ctest --test-dir /tmp/zlynx-static --output-on-failure -j4

clang-tidy znet/src/server/tcp_server.cc -p build/debug
git diff --check
```

安装消费按 README 的 `cmake --install` / `find_package` 入口进行，验证了共享与静态
两种导出。运行日志保存在本次工作环境的 `/tmp/zlynx-refactor-*.log`。

已完成吞吐、延迟、RSS 和持续运行测量，结果概要见上方验收表。
独立压测报告和原始数据附件未纳入仓库。部分场景的基线也出现明显性能漂移，不能据此宣称普遍提升；
60 秒持续运行不替代长期稳定性验证。未重新生成覆盖率报告，也未运行 sanitizer。
历史 `coverage/znet-summary.txt` 不代表新架构覆盖率。已验证平台及编译器限于上述环境。
