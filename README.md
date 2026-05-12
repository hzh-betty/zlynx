# zlynx

`zlynx` 是一个 Linux C++ 学习项目，也是一组从底层到上层逐步搭出来的高性能网络组件。
它的目标不是替代成熟生产库，而是把 Linux C++ 学习阶段的大多数核心知识串成一个可以阅读、
调试、测试、压测和扩展的完整工程。

如果你已经学过 C++ 语法、数据结构与算法、Linux 系统编程、网络编程、内存管理、
设计模式、CMake 和测试，这个项目可以把这些知识放在同一条链路里重新走一遍：
从 allocator，到日志，到协程，到 TCP 网络，再到 HTTP/WebSocket 服务。

## 模块概览

```text
zhttp   HTTP/WebSocket 框架层
  -> znet   TCP/TLS 网络层
      -> zco    协程运行时、hook、同步原语
          -> zlog   日志基础设施

zmalloc 可选全局 allocator override，也可显式使用 zmalloc/zfree
```

模块文档：

- [zlog](zlog/README.md)：日志模块，参考 `spdlog`
- [zmalloc](zmalloc/README.md)：内存分配器，参考 `tcmalloc`
- [zco](zco/README.md)：协程运行时，参考 `coost`
- [znet](znet/README.md)：协程 TCP 网络库，参考 `muduo`
- [zhttp](zhttp/README.md)：HTTP/WebSocket 框架，参考 `dragon`

## 设计参考

`zlynx` 会借鉴成熟项目的思想，但并不是逐行复刻：

- `zlog` 参考 `spdlog`：学习 logger、formatter、sink、同步/异步日志、双缓冲策略，
  以及单例、工厂、建造者等常见模式。
- `zmalloc` 参考 `tcmalloc`：学习对象池、thread cache、central cache、page cache、
  size class、span、page map 和 allocator override 等内存池策略。
- `zco` 参考 `coost`：学习 GMP 模型、协程调度、共享栈/独立栈、hook、事件等待、
  定时器、`Channel`/`WaitGroup` 等同步原语和工作窃取队列。
- `znet` 参考 `muduo`：学习 TCP server、connection、buffer、acceptor、回调模型和 TLS。
  `znet` 同时引入 actor/信箱模型，让连接的读、写、关闭事件在连接内部串行化，
  以降低多线程场景下的锁竞争。
- `zhttp` 参考 `dragon` 框架：学习路由、中间件、请求/响应抽象、静态文件、压缩、
  限流、Session、HTTPS 和 WebSocket。

## 能学到什么

这个仓库覆盖的知识点很密：

- C++14 工程组织、target 级 CMake、install/export/find_package
- RAII、智能指针、类型擦除、设计模式
- 多线程、原子变量、锁、条件变量、自旋锁、线程本地缓存
- 高性能日志、格式化、异步生产者/消费者、双缓冲区设计
- 内存分配器、对象池、内存池、全局 new/delete override
- 协程调度器、GMP 模型、上下文切换、共享栈、工作窃取、定时器、epoll、系统调用 hook
- TCP server、socket、acceptor、connection 状态机、buffer、TLS、actor 模型
- 路由、中间件、请求/响应抽象、multipart、断点续传、WebSocket
- GTest、CMock、测试覆盖率、benchmark、wrk、perf、valgrind
- 性能优化思路：
  - 数据结构：例如用基数树减少锁竞争、优化路由查找。
  - 缓存局部性：例如优化 Cache/内存布局、自旋锁减少Cache Miss。
  - CPU 执行效率：例如利用分支预测减少热点路径开销。
  - 设计策略：双缓冲区、actor模型减少锁竞争，提高并发。
  - 内存管理：例如对象池、减少拷贝、控制生命周期，以及优化日志消息。
  - 系统开销：例如减少系统调用、批处理、零拷贝和缓存日志时间格式化结果。
  - IO 策略：例如缓存常用静态文件、顺序化访问、减少磁盘访问。
  - 其他：用模板代替`std::function` 减少`std::function` 的间接调用开销。

## 构建

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

性能目标：

```bash
cmake --preset perf
cmake --build --preset perf
```

手动配置：

```bash
cmake -S . -B build/debug -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON
cmake --build build/debug -j
```

## 测试

运行全部 CTest：

```bash
ctest --test-dir build/debug --output-on-failure
```

按模块运行：

```bash
cmake --build --preset debug --target zlog_test
cmake --build --preset debug --target zmalloc_test
cmake --build --preset debug --target zco_test
cmake --build --preset debug --target znet_test
cmake --build --preset debug --target zhttp_test
```

每个模块还提供 `_test_unit` 和 `_test_integration` 目标，例如：

```bash
cmake --build --preset debug --target zhttp_test_unit
cmake --build --preset debug --target zhttp_test_integration
```

## 覆盖率

统一脚本会按模块生成 summary、branch 报告和 HTML 明细：

```bash
coverage/run_coverage.sh
```

当前仓库保留了各模块 summary：

| module | lines | functions | branches | decisions |
|---|---:|---:|---:|---:|
| zlog | 75.4% | 80.5% | 66.1% | 60.9% |
| zmalloc | 97.5% | 100.0% | 90.2% | 97.1% |
| zco | 96.6% | 100.0% | 87.6% | 88.7% |
| znet | 92.2% | 97.3% | 80.5% | 87.3% |
| zhttp | 94.8% | 98.3% | 85.1% | 84.3% |

## 性能

仓库把性能目标和常规测试分开：unit/integration 进入 CTest，benchmark/perf 目标需要
`ZLYNX_BUILD_PERF_TESTS=ON`，也就是使用 `perf` preset。

```bash
cmake --preset perf
cmake --build --preset perf --target zlog_performance
cmake --build --preset perf --target zmalloc_benchmark
cmake --build --preset perf --target zmalloc_performance
cmake --build --preset perf --target zco_performance
cmake --build --preset perf --target znet_wrk_benchmark
cmake --build --preset perf --target zhttp_benchmark
```

性能脚本位于各模块 `tests/benchmark/` 下，常见工具包括 `wrk`、`perf`、`valgrind` 和
`cachegrind`。性能数字强依赖机器、内核、编译器、第三方库、线程数、fd 限制和压测参数；
这个项目更适合作为观察优化方向和理解瓶颈的实验台。

## 安装与消费

每个模块都导出独立 CMake package：

```cmake
find_package(zlog CONFIG REQUIRED)
find_package(zmalloc CONFIG REQUIRED)
find_package(zco CONFIG REQUIRED)
find_package(znet CONFIG REQUIRED)
find_package(zhttp CONFIG REQUIRED)
```

对应 target：

```cmake
zlog::zlog
zmalloc::zmalloc
zmalloc::override
zco::zco
znet::znet
zhttp::zhttp
```

## 学习顺序

建议按依赖从下往上读：

1. `zlog`：先理解工程结构、fmt、sink、formatter、异步 looper 和测试。
2. `zmalloc`：理解 allocator 的分层设计，再看 override 如何接入整个工程。
3. `zco`：理解协程运行时、同步原语、定时器、hook 和共享栈。
4. `znet`：理解 TCP server 如何建立在协程运行时上，以及连接邮箱模型如何串行化事件。
5. `zhttp`：理解 HTTP 框架如何复用网络层，并通过 router/middleware 组织业务能力。

每一层都可以单独构建、单独测试、单独压测；把五层连起来看，则是一条完整的 Linux C++
网络服务学习路线。
