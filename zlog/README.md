# zlog

zlog 是基于 fmt 的同步/异步日志库，支持等级过滤、格式串、多输出目标和按大小
滚动文件。生产库最低使用 C++11，依赖 fmt 和 Threads。

## 最小示例

```cpp
#include "zlog/zlog.h"

int main() {
    zlog::LoggerBuilder builder;
    builder.build_logger_type(zlog::LoggerType::LOGGER_SYNC);
    builder.build_logger_name("app");
    builder.build_logger_level(zlog::LogLevel::value::INFO);
    builder.build_logger_formatter("[%p] %m%n");
    builder.build_logger_sink<zlog::StdOutSink>();

    auto logger = builder.build();
    logger->ZLOG_INFO("hello {}", "zlynx");
    logger->flush();
    logger->close();
    return 0;
}
```

`build()` 返回局部日志器；需要按名称查询时，通过 `LoggerManager` 显式注册。
root 宏使用管理器提供的默认同步日志器。日志器持有名称和格式配置，等级包括
DEBUG、INFO、WARNING、ERROR、FATAL、OFF。

## 输出与生命周期

- `SyncLogger` 在调用线程输出；`AsyncLogger` 把序列化记录交给后台线程。
- `StdOutSink`、`FileSink`、`RollBySizeSink` 分别用于标准输出、普通文件和滚动文件。
  一个日志器可配置多个 sink，内置 sink 支持多个日志器共享。
- `flush()` 等待此前已接收的记录输出并刷新 sink；`close()` 排空并关闭日志器，
  关闭后拒绝新记录。刷新不等同于磁盘持久化。
- 异步后台输出异常会在显式刷新或关闭时报告。需要观察失败时显式调用
  `flush()` / `close()`，不要只依赖析构。
- 自定义 sink 的 `log()` 和 `flush()` 不得重入同一个日志器。

异步日志通过 `LOGGER_ASYNC` 选择，默认使用固定容量缓冲；调用
`build_enable_unsafe()` 可允许缓冲扩容。两种模式达到容量上限时都会等待消费者，
超出单缓冲容量的单条记录会被拒绝。`build_wait_time()` 的等待时间必须为正数。

滚动 sink 按完整记录检查大小并限制保留文件数，超过单文件上限的单条记录会被拒绝。
同一 basename 应共用一个滚动 sink。文件 sink 默认不启用标准库文件缓冲，
可通过额外的 `FileBufferMode::BUFFERED` 参数显式启用；启用后需考虑刷新时机及延迟报错。

## 构建与测试

以下命令从 zlynx 根目录执行，需要 CMake 3.18+。默认开启测试，测试需要
GTest/GMock 和支持 C++17 的编译器：

```bash
cmake -S zlog -B build/zlog -DCMAKE_BUILD_TYPE=Debug
cmake --build build/zlog -j 2
(cd build/zlog && ctest --output-on-failure)
```

仅构建生产库可加 `-DBUILD_TESTING=OFF`；共享库可加 `-DBUILD_SHARED_LIBS=ON`。
整体构建与预设用法见[根 README](../README.md)，整体构建后用
`ctest -R '^zlog_'` 筛选本模块测试。测试覆盖格式化、等级过滤、sink、刷新关闭、
异步缓冲、共享所有权和并发输出。

## 安装与使用

```bash
cmake --install build/zlog --prefix /path/to/prefix
```

消费者配置时传入 `-DCMAKE_PREFIX_PATH=/path/to/prefix`：

```cmake
find_package(zlog 1 CONFIG REQUIRED)
add_executable(app main.cc)
target_link_libraries(app PRIVATE zlog::zlog)
```

安装包会查找 fmt 和 Threads，并传递公共依赖。

## 性能与源码

```bash
cmake --preset relwithdebinfo -DBUILD_BENCHMARKS=ON
cmake --build --preset relwithdebinfo --target zlog_performance -j 2
build/relwithdebinfo/zlog/tests/zlog_performance \
  -m both -t 4 -c 100000 -s 128 -o build/zlog-perf-logs
```

以上命令从仓库根目录执行。`zlog_performance` 对比同步/异步输出并计入刷新耗时；
安装 spdlog 后还可构建 `zlog_benchmark`。性能结果受消息长度、sink、文件缓冲、
线程数和存储设备影响。源码和 perf 脚本位于 [tests/benchmark](tests/benchmark)，
覆盖率入口见[根 README](../README.md)。

公开接口位于 [include/zlog](include/zlog)，日志器与 sink 实现位于 [src](src)，
异步缓冲和消费循环位于 [src/async](src/async)。
