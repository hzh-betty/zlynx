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
      -> zco    协程运行时、就绪等待、同步原语
  -> zlog   应用日志基础设施

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
  `znet` 用 ByteStream 隔离 TCP/TLS，读写分别串行化，协议状态由会话持有。
- `zhttp` 参考 `dragon` 框架：学习路由、中间件、请求/响应抽象、静态文件、压缩、
  限流、Session、HTTPS 和 WebSocket。

## 能学到什么

这个仓库覆盖的知识点很密：

- C++17 工程组织、target 级 CMake、install/export/find_package
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

所有项目版本统一为 **1.0.0**。需要 CMake 3.18+、Linux、C++ 编译器；
`zlog`/`zmalloc` 最低 C++11，其余模块最低 C++17。依赖由系统包或用户工具链提供：
fmt、OpenSSL 1.1+、zlib、nlohmann_json、toml11、Brotli 开发包及 pkg-config。
测试还需要 GTest/GMock，TLS 集成测试需要 `openssl` 命令。

构建脚本支持 CMake 3.18；该版本使用普通命令构建与测试：

```bash
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug -j 2
(cd build/debug && ctest --output-on-failure)
```

Presets 需要 CMake 3.20+，只有三种，不指定编译器，默认生成静态库并开启测试：

```bash
cmake --preset debug
cmake --build --preset debug -j 2
ctest --preset debug

cmake --preset release
cmake --build --preset release -j 2
ctest --preset release

cmake --preset relwithdebinfo
cmake --build --preset relwithdebinfo -j 2
ctest --preset relwithdebinfo
```

共享库增加 `-DBUILD_SHARED_LIBS=ON`；只构建产品库增加 `-DBUILD_TESTING=OFF`。
切换旧构建结构时使用新目录，不复用旧缓存。个人编译器、工具链等放入不提交的
`CMakeUserPresets.json`，或配置时传递 `-DCMAKE_CXX_COMPILER=clang++`。

### 独立源码包

每个模块的整个目录都可以复制到仓库外，包含自己的 CMake 和安装模板，
不需要顶层目录或 `../cmake`。例如在复制后的 zlog 目录执行：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 2
(cd build && ctest --output-on-failure)
cmake --install build --prefix /path/to/prefix
```

zmalloc、zlog、zco 仅需要系统依赖。znet 先安装 zco；zhttp 先安装 zco、znet、zlog，
配置时通过 `-DCMAKE_PREFIX_PATH=/path/to/prefix` 指向它们。无需复制兄弟目录。

## 测试

CTest 直接运行全部测试，测试名与 executable 的模块前缀一致：

```bash
ctest --preset debug
(cd build/debug && ctest -R '^zhttp_' --output-on-failure)
```

不再提供 `_test_unit`、`_test_integration` 等自定义目标；缺少 GTest 时配置失败。
多配置生成器使用 `cmake --build <build> --config Debug` 和 `(cd <build> && ctest -C Debug)`。

## 性能与覆盖率

常规测试和性能程序分开，性能程序不注册到 CTest：

```bash
cmake --preset relwithdebinfo -DBUILD_BENCHMARKS=ON
cmake --build --preset relwithdebinfo -j 2
```

保留各模块的 benchmark/performance executable；`zlog_benchmark` 对比程序额外需要
spdlog，缺失时明确提示，其他性能程序仍可构建。zco 显式开启 benchmark 时才启用
现有栈复制统计。perf/wrk/valgrind 脚本仍位于各模块的 `tests/benchmark/`。

覆盖率通过 `coverage/run_coverage.sh` 使用标准 CMake 编译/链接参数生成，不再有
coverage 预设或项目 coverage target。`coverage/*-summary.txt` 是历史测量数据，
不代表本次构建的覆盖率。Sanitizer 同样使用单独构建目录，例如：

```bash
cmake -S . -B build/ubsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_FLAGS=-fsanitize=undefined \
  -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=undefined \
  -DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=undefined
cmake --build build/ubsan -j 2
(cd build/ubsan && UBSAN_OPTIONS=halt_on_error=1 ctest --output-on-failure)
```

ASan 接管 malloc/free，与显式全局 allocator 替换测试冲突；CMake 不禁用这些测试。
独立 zlog 等不替换 allocator 的模块可以使用 `-fsanitize=address` 正常验证。

## 安装与消费

每个模块独立导出 Config Package，整体构建也可一次安装：

```bash
cmake --install build/release --prefix /path/to/prefix
```

消费者通过 `CMAKE_PREFIX_PATH` 找到安装前缀：

```cmake
find_package(zhttp 1 CONFIG REQUIRED)
add_executable(app main.cc)
target_link_libraries(app PRIVATE zhttp::zhttp)
```

其他核心 target 是 `zlog::zlog`、`zmalloc::zmalloc`、`zco::zco`、`znet::znet`。
全局 allocator 替换由最终程序显式链接 `zmalloc::override`；顶层不再自动注入。
`zhttp::core` 已合并到 `zhttp::zhttp`。所有共享库 SONAME 主版本改为 1，旧消费者
必须重新链接；安装到新前缀以避免旧主版本、旧 core 库及 Config 混杂。

安装包不强制修改动态加载路径。共享库部署时将安装 libdir 配置到系统加载器，
或由应用设置相对其部署布局的 RPATH；测试临时前缀也可以设置 `LD_LIBRARY_PATH`。

修改前分析与最终验证见 [CMake 审计](docs/cmake-audit.md) 和
[CMake 重构报告](docs/cmake-refactor.md)。

## 学习顺序

建议按依赖从下往上读：

1. `zlog`：先理解工程结构、fmt、sink、formatter、异步 looper 和测试。
2. `zmalloc`：理解 allocator 的分层设计，再看 override 如何接入整个工程。
3. `zco`：理解协程运行时、同步原语、定时器、hook 和共享栈。
4. `znet`：理解 TCP server 如何建立在协程运行时上，以及连接邮箱模型如何串行化事件。
5. `zhttp`：理解 HTTP 框架如何复用网络层，并通过 router/middleware 组织业务能力。

每一层都可以单独构建、单独测试、单独压测；把五层连起来看，则是一条完整的 Linux C++
网络服务学习路线。
