# zlog

`zlog` 是 zlynx 的日志模块，整体设计参考 `spdlog` 的易用接口和 sink/formatter
组合方式，提供同步日志、异步日志、格式化、日志落地器和全局 logger 管理能力。
它是整个 zlynx 项目的基础设施模块，也可以独立作为轻量级 C++11 日志库使用。

## 快速开始

使用 builder 创建局部 logger：

```cpp
#include "zlog/zlog.h"

int main() {
    zlog::LoggerBuilder builder;
    builder.build_logger_name("demo");
    builder.build_logger_type(zlog::LoggerType::LOGGER_SYNC);
    builder.build_logger_level(zlog::LogLevel::value::DEBUG);
    builder.build_logger_formatter("[%d{%H:%M:%S}][%t][%p] %m%n");
    builder.build_logger_sink<zlog::StdOutSink>();

    auto logger = builder.build();
    logger->ZLOG_INFO("hello {}", "zlog");
    logger->ZLOG_WARN("answer={}", 42);
    logger->close();
    return 0;
}
```

创建全局 logger，并通过名称获取：

```cpp
#include "zlog/zlog.h"

int main() {
    zlog::LoggerBuilder builder;
    builder.build_logger_name("app");
    builder.build_logger_type(zlog::LoggerType::LOGGER_ASYNC);
    builder.build_logger_level(zlog::LogLevel::value::INFO);
    builder.build_logger_formatter("[%d{%H:%M:%S}][%c][%p] %m%n");
    builder.build_wait_time(std::chrono::milliseconds(50));
    builder.build_logger_sink<zlog::FileSink>("app.log");
    auto logger = builder.build();
    zlog::LoggerManager::get_instance().add_logger(logger);

    zlog::get_logger("app")->ZLOG_INFO("server started on port={}", 8080);
    logger->close();
    return 0;
}
```

安装后消费：

```cmake
cmake_minimum_required(VERSION 3.18)
project(zlog_demo LANGUAGES CXX)

find_package(zlog CONFIG REQUIRED)

add_executable(zlog_demo main.cc)
target_link_libraries(zlog_demo PRIVATE zlog::zlog)
```

源码树内开发可以直接链接 `zlog` target。

原来的 `LocalLoggerBuilder` / `GlobalLoggerBuilder` 已合并为 `LoggerBuilder`：
构建使用 `build()`，需要全局查询时显式调用 `LoggerManager::add_logger()`。
`build_global()` 已删除；注册空指针或重名日志器会抛出 `std::invalid_argument`。
`SinkFactory::create<T>()` 改为 `std::make_shared<T>()`，单独的格式项类改由
`Formatter` 的格式规则表达。公开类型和内部布局有变化，依赖方需要迁移并重新编译。

`ModuleLogger` 及其依赖初始化回调已删除，调用方使用 `LoggerBuilder` 显式构建，
需要全局注册时调用 `LoggerManager::add_logger()`。`LoggerManager::upsert_logger()` 已删除，
注册表只保留添加和查询接口，不再支持替换日志器。

本轮按架构评审完成接口边界与所有权调整。使用 `zlog/zlog.h` 的代码仍可通过
聚合入口访问公共接口；使用单独头文件的代码应按职责包含：

```cpp
#include "zlog/logger.h"          // Logger、SyncLogger、AsyncLogger 与 AsyncType
#include "zlog/logger_builder.h"  // LoggerBuilder 与 LoggerType
#include "zlog/logger_registry.h" // LoggerManager
```

Logger 和 Builder 复制并持有名称；修改或销毁调用方的名称字符串不会影响已有
配置和日志器。Builder 未设置名称时构建返回空指针，显式设置空字符串仍可构建；
`build_logger_name(nullptr)` 清除名称配置。

源码与安装接口的最低标准为 C++11，允许使用 C++17 等更高标准编译。
`LoggerBuilder::build_logger_name(fmt::string_view)` 按长度复制名称，视图只需在
该次调用期间有效。日志格式串、LogMessage 正文及名称使用 `fmt::string_view`
借用短期存储；格式化与内置 sink 按指定长度处理数据，保留内嵌 NUL，不要求
输入零终止。Logger 名称、Builder 配置和 Formatter 的 pattern 仍拥有自己的字符串。

Buffer、AsyncLooper 和 Spinlock 移入 `zlog::detail` 与私有源码目录，不再安装
对应头文件；NonCopyable 已删除，类型通过删除复制操作表达约束。依赖旧内部
类型或旧 ABI 的调用方需要迁移并重新编译。本轮新增虚函数和成员布局，
共享库版本已升为 2.0.0（SONAME 为 `libzlog.so.2`）。实施范围和验证见
[重构实施记录](docs/refactoring-report.md)。

## 刷新、关闭与错误

同步和异步日志器都提供公开的 `flush()` 和 `close()`。同步 `flush()` 刷新所有
sink；异步 `flush()` 等待本次请求前已入队的记录输出完成，再刷新所有 sink。
刷新把用户态缓冲交给操作系统，不等于 `fsync` 的断电持久性保证。

`close()` 停止接收新日志，异步模式会排空队列、刷新并等待工作线程结束，
然后释放该 logger 持有的 sink。重复关闭无操作；关闭后写入抛出异常。
共享 sink 由引用计数管理，关闭一个 logger 不会关闭其他 logger 持有的 sink。
内置 sink 自身加锁；自定义 sink 若被多个 logger 共用，也须同步 `log()` 和 `flush()`。
sink 回调中重入同一 logger 会抛出异常，避免锁等待或队列自阻塞。

打开、写入和刷新失败会报告异常。多 sink 输出会尝试所有 sink，再报告首个错误；
异步后台失败不会阻止后续记录和其他 sink 的输出，首个后台错误由 `flush()` 或
`close()` 重抛。关闭即使报告错误也完成清理。析构不抛出，未显式关闭时的错误
会输出到标准错误，因此需要处理错误的调用方应显式调用 `close()`。

`RollBySizeSink(basename, max_size, auto_flush = false, max_files = 10)` 默认保留
至多 10 个匹配的滚动文件（含当前文件），启动时也清理旧文件；保留数量可配置，
大小与数量必须大于零。超过 `max_size` 的单条序列化日志会被拒绝并报错，
不会拆分或写入超限文件。异步模式也按单条记录轮转。相同 basename 应由一个
共享 sink 管理，避免多个独立 sink 的保留策略互相影响。

异步 safe/unsafe 的单缓冲容量分别为 2 MiB / 512 MiB；每条记录的长度头也计入
容量，单条超限会立即报错，容量不足时等待消费者。异步等待时间必须为正数。
时间格式化继续按线程、秒缓存，缓存同时校验格式串，避免不同格式混用结果。

正文和序列化缓冲区也继续通过 `thread_local` 复用容量。普通调用独占借用缓存，
结束后只清空内容；缓冲区已经被外层调用占用时，嵌套日志才使用临时缓冲区，
异常退出也会归还占用标记。LogMessage 保持局部对象，借用当前调用的正文。

## 项目架构

`zlog` 是 zlynx 的最底层公共模块之一：

```text
zlog
  -> fmt       格式化库
  -> Threads   异步 looper 和并发写入

zhttp runtime
  -> zlog      每实例同步日志器
```

核心目录：

```text
zlog/
  include/zlog/              公共 API：logger、builder、registry、sink、formatter 等
  include/zlog/internal/     待后续阶段收回的 File 工具
  src/                       模块实现
  src/async/                 私有 Buffer、AsyncLooper 与 Spinlock
  tests/unit/                单元测试
  tests/integration/         端到端与多线程集成测试
  tests/benchmark/           benchmark、perf 脚本和第三方对比入口
```

主要组件：

- `Logger`：同步/异步 logger 的抽象基类，负责等级过滤、fmt 格式化和消息序列化。
- `SyncLogger`：调用线程内直接落地日志，适合简单场景或需要在调用线程完成 sink 输出的路径。
- `AsyncLogger`：将序列化后的日志写入 `AsyncLooper`，由后台线程批量收取、逐条落地。
- `zlog::detail::AsyncLooper`：内部生产者/消费者实现，由 AsyncLogger 独占；safe 使用固定容量，unsafe 允许扩容，两种模式达到容量上限时均等待可用空间。
- `Formatter`：以格式项值保存规则，统一解析和执行 `%d`、`%t`、`%c`、`%f`、`%l`、`%p`、`%T`、`%m`、`%n` 等格式项。
- `LogSink`：日志落地抽象，内置 `StdOutSink`、`FileSink`、`RollBySizeSink`。
- `LoggerBuilder`：用 builder 方式组装 logger 类型、名称、等级、格式、sink 和异步参数；`build()` 构建，注册由调用方显式完成。
- `LoggerManager`：全局 logger 注册表，提供 root logger 和命名 logger 查询。

## 依赖

基础构建依赖：

- CMake 3.18+
- C++11 或更高标准的编译器，仓库 preset 使用 `clang++` 和 C++17
- Ninja，使用 preset 时需要
- fmt
- Threads

测试和分析额外依赖：

- GTest / GMock
- `spdlog`：只用于 `zlog_benchmark` 第三方对比；缺失时跳过该目标
- `gcovr`
- `perf`

测试框架可以要求更高的编译标准。单独配置 zlog 时可通过
`-DCMAKE_CXX_STANDARD=11` 验证生产库，通过 `-DCMAKE_CXX_STANDARD=17` 选择更高标准。

## 编译

推荐在仓库根目录使用 CMake presets。

```bash
cmake --preset debug
cmake --build --preset debug
```

发布构建：

```bash
cmake --preset release
cmake --build --preset release
```

构建性能测试二进制：

```bash
cmake --preset perf
cmake --build --preset perf --target zlog_performance
```

如果系统安装了 `spdlog`，还可以构建第三方对比 benchmark：

```bash
cmake --build --preset perf --target zlog_benchmark
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

安装后导出 `zlog::zlog`，并通过包配置转发 `fmt` 和 `Threads` 依赖。

## 测试

运行全部 zlog 测试：

```bash
cmake --build --preset debug --target zlog_test
```

只跑单元测试：

```bash
cmake --build --preset debug --target zlog_test_unit
```

只跑集成测试：

```bash
cmake --build --preset debug --target zlog_test_integration
```

直接使用 CTest：

```bash
ctest --test-dir build/debug -R '^zlog\.' --output-on-failure
ctest --test-dir build/debug -R '^zlog\.unit\.' --output-on-failure
ctest --test-dir build/debug -R '^zlog\.integration\.' --output-on-failure
```

当前测试覆盖的主要行为：

- Buffer 扩容、读写索引、交换和边界条件
- 日志等级字符串转换和等级过滤
- Formatter 格式项解析与输出
- StdOut/File/RollBySize sink 的构造和输出
- SyncLogger、AsyncLogger、空 sink、异常路径
- LoggerBuilder 局部构建、全局注册以及 LoggerManager 添加/查询
- AsyncLooper safe/unsafe 模式、flush 阈值、stop 和析构
- 多 sink、滚动文件、多线程同步/异步写入、端到端日志内容校验
- 重入、共享 sink、公开刷新与关闭、后台异常、严格滚动大小和文件保留数量

## 覆盖率

统一脚本：

```bash
coverage/run_coverage.sh
```

只生成报告、不重新跑测试：

```bash
coverage/run_coverage.sh --no-test
```

`coverage/zlog-summary.txt` 中记录的当前 zlog 覆盖率：

| 指标 | 覆盖率 |
|---|---:|
| Lines | 75.4% (356 / 472) |
| Functions | 80.5% (62 / 77) |
| Branches | 66.1% (222 / 336) |
| Decisions | 60.9% (92 / 151) |

覆盖率报告只统计 `zlog/src`。

## 性能

`zlog_performance` 支持同步、异步或两者对比，输出总消息数、含刷新耗时、吞吐和
吞吐倒数（不代表单条调用延迟）。同步和异步使用相同的 `auto_flush = false`，
计时结束前等待全部已接收日志输出及刷新。

```bash
cmake --preset perf
cmake --build --preset perf --target zlog_performance

build/perf/zlog/tests/zlog_performance \
  -m both \
  -t 4 \
  -c 1000000 \
  -s 128 \
  -o perf_bench_logs
```

参数：

```bash
-m <mode>      sync | async | both，默认 both
-c <count>     日志条数，默认 1000000
-t <threads>   线程数，默认 4
-d <duration>  运行时长，0 表示按条数运行
-s <size>      消息大小，默认 128 字节
-o <output>    输出目录，默认 perf_bench_logs
```

脚本入口：

```bash
cd build/perf/zlog/tests
../../../../zlog/tests/benchmark/zlog_perf.sh both 4 1000000 128
```

`zlog_perf.sh` 会使用 `perf record` 和 `perf stat` 采集 CPU 采样、缓存统计和 benchmark
日志。性能结果受磁盘、文件系统、auto flush、消息大小、线程数、fmt 版本和 sink
类型影响，应在同一机器和同一参数下比较。

## 支持功能

- 同步日志器和异步日志器
- 异步 safe/unsafe 两种缓冲策略
- fmt 风格参数格式化
- 日志等级：DEBUG、INFO、WARNING、ERROR、FATAL、OFF
- 等级过滤
- pattern formatter
- 标准输出、普通文件、按大小滚动文件 sink
- 多 sink 同时落地
- Local logger 和 Global logger
- Root logger 和命名 logger 查询
- Logger builder 配置入口
- 多线程写入
- 第三方日志库 benchmark 对比入口
