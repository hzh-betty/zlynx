# zlog

`zlog` 是 zlynx 的日志模块，整体设计参考 `spdlog` 的易用接口和 sink/formatter
组合方式，提供同步日志、异步日志、格式化、日志落地器和全局 logger 管理能力。
它是整个 zlynx 项目的基础设施模块，也可以独立作为轻量级 C++14 日志库使用。

## 快速开始

使用 builder 创建局部 logger：

```cpp
#include "zlog/zlog.h"

int main() {
    zlog::LocalLoggerBuilder builder;
    builder.build_logger_name("demo");
    builder.build_logger_type(zlog::LoggerType::LOGGER_SYNC);
    builder.build_logger_level(zlog::LogLevel::value::DEBUG);
    builder.build_logger_formatter("[%d{%H:%M:%S}][%t][%p] %m%n");
    builder.build_logger_sink<zlog::StdOutSink>();

    auto logger = builder.build();
    logger->ZLOG_INFO("hello {}", "zlog");
    logger->ZLOG_WARN("answer={}", 42);
    return 0;
}
```

创建全局 logger，并通过名称获取：

```cpp
#include "zlog/zlog.h"

int main() {
    zlog::GlobalLoggerBuilder builder;
    builder.build_logger_name("app");
    builder.build_logger_type(zlog::LoggerType::LOGGER_ASYNC);
    builder.build_logger_level(zlog::LogLevel::value::INFO);
    builder.build_logger_formatter("[%d{%H:%M:%S}][%c][%p] %m%n");
    builder.build_wait_time(std::chrono::milliseconds(50));
    builder.build_logger_sink<zlog::FileSink>("app.log");
    builder.build();

    zlog::get_logger("app")->ZLOG_INFO("server started on port={}", 8080);
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

## 项目架构

`zlog` 是 zlynx 的最底层公共模块之一：

```text
zlog
  -> fmt       格式化库
  -> Threads   异步 looper 和并发写入

zco / znet / zhttp
  -> zlog      复用日志接口和日志级别
```

核心目录：

```text
zlog/
  include/zlog/              公共 API：logger、sink、formatter、level 等
  include/zlog/internal/     Buffer、AsyncLooper、工具函数
  src/                       模块实现
  tests/unit/                单元测试
  tests/integration/         端到端与多线程集成测试
  tests/benchmark/           benchmark、perf 脚本和第三方对比入口
```

主要组件：

- `Logger`：同步/异步 logger 的抽象基类，负责等级过滤、fmt 格式化和消息序列化。
- `SyncLogger`：调用线程内直接落地日志，适合简单场景或对退出前持久化要求高的路径。
- `AsyncLogger`：将序列化后的日志写入 `AsyncLooper`，由后台线程批量落地。
- `AsyncLooper`：生产者/消费者模型，维护生产缓冲区和消费缓冲区，支持 safe/unsafe 两种模式。
- `Formatter`：解析 `%d`、`%t`、`%c`、`%f`、`%l`、`%p`、`%T`、`%m`、`%n` 等格式项。
- `LogSink`：日志落地抽象，内置 `StdOutSink`、`FileSink`、`RollBySizeSink`。
- `LoggerBuilder`：用 builder 方式组装 logger 类型、名称、等级、格式、sink 和异步参数。
- `LoggerManager`：全局 logger 注册表，提供 root logger 和命名 logger 查询。

## 依赖

基础构建依赖：

- CMake 3.18+
- C++14 编译器，仓库 preset 默认使用 `clang++`
- Ninja，使用 preset 时需要
- fmt
- Threads

测试和分析额外依赖：

- GTest / GMock
- `spdlog`、`glog`：只用于 `zlog_benchmark` 第三方对比；缺失时跳过该目标
- `gcovr`
- `perf`

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

如果系统安装了 `spdlog` 和 `glog`，还可以构建第三方对比 benchmark：

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
- StdOut/File/RollBySize sink 和 SinkFactory
- SyncLogger、AsyncLogger、空 sink、异常路径
- LocalLoggerBuilder、GlobalLoggerBuilder、LoggerManager 注册/替换/查询
- AsyncLooper safe/unsafe 模式、flush 阈值、stop 和析构
- 多 sink、滚动文件、多线程同步/异步写入、端到端日志内容校验

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

`zlog_performance` 支持同步、异步或两者对比，输出总消息数、耗时、吞吐和平均延迟。

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
