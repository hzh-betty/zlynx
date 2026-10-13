# zlynx

zlynx 是一个面向 Linux 的 C++ 学习项目，由内存分配、日志、协程、网络和
HTTP/WebSocket 五个模块组成。各模块提供独立的 CMake 构建、测试和安装入口，
也可以在仓库根目录一起构建。

## 模块

| 模块 | 职责 | 最低 C++ 标准 | 主要依赖 |
| --- | --- | --- | --- |
| [zmalloc](zmalloc/README.md) | 分层内存分配器、显式分配接口、可选全局替换 | C++11 | Threads |
| [zlog](zlog/README.md) | 同步/异步日志、格式化、文件滚动 | C++11 | fmt、Threads |
| [zco](zco/README.md) | 有栈协程、任务调度、同步原语、描述符 I/O | C++17 | Linux、Threads |
| [znet](znet/README.md) | TCP 服务、UDP socket、TLS 传输 | C++17 | zco、OpenSSL |
| [zhttp](zhttp/README.md) | HTTP 服务、路由、中间件、WebSocket | C++17 | znet、zco、zlog 及 HTTP 相关依赖 |

依赖关系：

```text
zhttp ──→ znet ──→ zco
  ├─────→ zco
  └─────→ zlog ──→ fmt

zmalloc：独立使用；全局替换由最终程序显式链接
```

当前各模块的 CMake 包版本均为 `1.0.0`。协程使用显式 `Runtime` 和 I/O 接口，
网络层管理传输与连接，HTTP 层持有协议和请求状态。

## 环境与依赖

整体构建需要 Linux、CMake 3.18+ 和支持 C++17 的编译器。使用仓库预设还需要
CMake 3.20+ 和 Ninja；预设不指定编译器。

外部开发依赖：

- fmt、OpenSSL 1.1+、zlib。
- nlohmann_json、toml11。
- pkg-config、Brotli 编码库（`libbrotlienc`）。
- 开启测试时需要 GTest/GMock、Brotli 解码库（`libbrotlidec`）和 `openssl` 命令。
- 可选工具：spdlog 用于日志对比测试，wrk 用于 HTTP 压测，gcovr 用于覆盖率报告。
- 若仅构建 znet 且环境缺少 OpenSSL，可配置 `-DZNET_ENABLE_TLS=OFF` 关闭 TLS 传输实现。

依赖需由系统或工具链提供，构建不会自动下载。单独构建模块时只需准备该模块的依赖。

## 构建与测试

以下命令从仓库根目录执行：

```bash
cmake --preset debug
cmake --build --preset debug -j 2
ctest --preset debug
```

预设包括 `debug`、`release`、`relwithdebinfo`，构建目录分别位于 `build/` 下。
默认构建静态库并启用测试。只构建库或改为共享库时，在配置命令后追加选项：

```bash
cmake --preset release -DBUILD_TESTING=OFF
cmake --build --preset release -j 2

cmake --preset debug -DBUILD_SHARED_LIBS=ON
cmake --build --preset debug -j 2
```

CMake 3.18 可使用普通配置命令：

```bash
cmake -S . -B build/manual -DCMAKE_BUILD_TYPE=Debug
cmake --build build/manual -j 2
(cd build/manual && ctest --output-on-failure)
```

按模块运行测试：

```bash
(cd build/debug && ctest -R '^zhttp_' --output-on-failure)
```

将前缀换成 `zmalloc_`、`zlog_`、`zco_` 或 `znet_` 可运行相应模块的测试。
测试程序使用 C++17。开启 `BUILD_TESTING` 而缺少测试依赖时，配置会失败。
编译器或生成器发生变化时使用新的构建目录；个人配置可放入被忽略的
`CMakeUserPresets.json`。

## 安装与接入

```bash
cmake --preset release -DBUILD_TESTING=OFF
cmake --build --preset release -j 2
cmake --install build/release --prefix /path/to/prefix
```

消费者通过 `-DCMAKE_PREFIX_PATH=/path/to/prefix` 查找安装包，例如：

```cmake
cmake_minimum_required(VERSION 3.18)
project(app LANGUAGES CXX)

find_package(zhttp 1 CONFIG REQUIRED)
add_executable(app main.cc)
target_link_libraries(app PRIVATE zhttp::zhttp)
```

其他模块的包名与目标为 `zmalloc` / `zmalloc::zmalloc`、`zlog` / `zlog::zlog`、
`zco` / `zco::zco`、`znet` / `znet::znet`。全局分配器替换单独链接
`zmalloc::override`；其他模块不会自动启用它。

每个模块目录都包含自己的构建文件和安装模板，可以复制到仓库外单独构建。
znet 需先安装 zco；zhttp 需先安装 zco、znet、zlog，并通过
`CMAKE_PREFIX_PATH` 指定安装前缀。共享库部署还需由应用或系统配置动态库搜索路径。

## 性能与覆盖率

性能程序默认关闭，启用后不会注册到 CTest：

```bash
cmake --preset relwithdebinfo -DBUILD_BENCHMARKS=ON
cmake --build --preset relwithdebinfo -j 2
```

| 模块 | 性能目标 |
| --- | --- |
| zmalloc | `zmalloc_benchmark`、`zmalloc_performance`、`thread_cache_benchmark` |
| zlog | `zlog_performance`、`zlog_benchmark`（需要 spdlog） |
| zco | `zco_performance` |
| znet | `znet_wrk_benchmark` |
| zhttp | `zhttp_benchmark`、`zhttp_core_benchmark` |

程序位于 `build/relwithdebinfo/<模块>/tests/`，对应源码和分析脚本位于各模块的
`tests/benchmark/`。性能比较需使用一致的构建模式、机器和参数。

[覆盖率脚本](coverage/run_coverage.sh) 会构建共享库、运行测试，并统计各模块的
`src/` 和 `include/`：

```bash
coverage/run_coverage.sh
```

默认报告目录为 `coverage/reports/`，汇总文件为其中的 `summary.md`。
`coverage/*-summary.txt` 是已有测量记录，不代表当前代码的覆盖率。

## 源码导航

每个模块的 `include/<模块>/` 提供公共接口，`src/` 保存实现，`tests/` 包含
单元、集成和性能测试，`cmake/` 提供安装包配置模板。可先阅读模块 README 和
对应测试，再沿 `zco → znet → zhttp` 追踪一次连接和请求的完整生命周期。
