# zco

`zco` 是 zlynx 的协程运行时模块，为上层 `znet`、`zhttp` 提供调度器、协程栈、
同步原语、定时器、epoll 与系统调用 hook。它面向需要在 Linux/C++14 中构建高并发
网络组件的开发者。

## 快速开始

```cpp
#include "zco/zco.h"

#include <iostream>

int main() {
    zco::co_stack_model(zco::StackModel::kIndependent);
    zco::co_stack_size(64 * 1024);
    zco::init(2);

    zco::WaitGroup wg(2);
    zco::Channel<int> ch(8);

    zco::go([&] {
        ch.write(42);
        wg.done();
    });

    zco::go([&] {
        int value = 0;
        if (ch.read(value)) {
            std::cout << "value=" << value << std::endl;
        }
        wg.done();
    });

    wg.wait();
    ch.close();
    zco::shutdown();
    return 0;
}
```

安装后消费：

```cmake
cmake_minimum_required(VERSION 3.18)
project(zco_demo LANGUAGES CXX)

find_package(zco CONFIG REQUIRED)

add_executable(zco_demo main.cc)
target_link_libraries(zco_demo PRIVATE zco::zco)
```

源码树内开发可以直接链接 `zco` target。

## 项目架构

`zco` 位于 zlynx 运行时底座层：

```text
zco
  -> zlog      日志输出
  -> Threads   pthread/std::thread 相关运行时依赖
  -> Linux     epoll、ucontext/上下文切换、socket/fd hook
```

核心目录：

```text
zco/
  include/zco/              公共 API：sched、channel、event、mutex、wait_group 等
  include/zco/internal/     fiber、processor、runtime、timer、epoller 等内部实现
  src/                      协程运行时实现
  tests/unit/               单元测试
  tests/integration/        hook 与运行时集成测试
  tests/benchmark/          调度、通道、定时器、hook 性能入口
```

主要组件：

- `sched`：`init()`、`go()`、`shutdown()`、`Scheduler`、栈模型和栈参数配置。
- `Fiber` / `Processor` / `RuntimeManager`：协程对象、工作线程、调度器生命周期与任务分发。
- `SharedStackBuffer` / `SnapshotBufferPool`：共享栈模型下的栈保存与复用。
- `StealQueue`：工作窃取队列，用于多调度器负载均衡。
- `Event`、`Mutex`、`WaitGroup`、`Channel<T>`：协程和线程可共享的同步原语。
- `Timer` / `Epoller` / `IoEvent`：定时等待与 I/O 事件唤醒。
- `hook`：对常见阻塞系统调用进行协程化等待，让网络 I/O 能让出调度器。

## 依赖

基础构建依赖：

- CMake 3.18+
- C++14 编译器，仓库 preset 默认使用 `clang++`
- Ninja，使用 preset 时需要
- Linux/POSIX，当前 CMake 明确拒绝非 UNIX 或 Apple 平台
- Threads
- `zlog`

测试和分析额外依赖：

- GTest / GMock
- `gcovr`，生成覆盖率报告
- `perf`、`valgrind`、`cg_annotate`、`callgrind_annotate`，可选性能分析工具

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
cmake --build --preset perf --target zco_performance
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

安装后导出 `zco::zco`，并通过包配置转发 `zlog` 和 `Threads` 依赖。

## 测试

运行全部 zco 测试：

```bash
cmake --build --preset debug --target zco_test
```

只跑单元测试：

```bash
cmake --build --preset debug --target zco_test_unit
```

只跑集成测试：

```bash
cmake --build --preset debug --target zco_test_integration
```

直接使用 CTest：

```bash
ctest --test-dir build/debug -R '^zco\.' --output-on-failure
ctest --test-dir build/debug -R '^zco\.unit\.' --output-on-failure
ctest --test-dir build/debug -R '^zco\.integration\.' --output-on-failure
```

当前测试覆盖的主要行为：

- 调度器生命周期、任务投递、指定调度器投递
- 协程创建、恢复、退出、句柄注册与清理
- 独立栈和共享栈、快照缓冲池、fiber pool
- work stealing queue、processor wait/timer
- `Event`、`Mutex`、`WaitGroup`、`Channel<T>`、`Pool`
- epoll poller、I/O event、timer queue
- hook helper、hook 超时元数据、socket/hook 集成路径
- runtime manager、日志、noncopyable 等基础组件

## 覆盖率

统一脚本：

```bash
coverage/run_coverage.sh
```

只生成报告、不重新跑测试：

```bash
coverage/run_coverage.sh --no-test
```

`coverage/zco-summary.txt` 中记录的当前 zco 覆盖率：

| 指标 | 覆盖率 |
|---|---:|
| Lines | 96.6% (1962 / 2032) |
| Functions | 100.0% (353 / 353) |
| Branches | 87.6% (1248 / 1425) |
| Decisions | 88.7% (432 / 487) |

覆盖率报告只统计 `zco/src`。

## 性能

`zco_performance` 覆盖调度吞吐、通道、定时器、hook 和不同协程栈模型。它输出
`[zco-perf] scenario=...` 摘要，便于脚本收集。

```bash
cmake --preset perf
cmake --build --preset perf --target zco_performance

build/perf/zco/tests/zco_performance
```

常用环境变量：

```bash
ZCO_PERF_SCALE_PCT=100
ZCO_PERF_SCHED_COUNT=8
ZCO_PERF_PRODUCER_THREADS=8
ZCO_PERF_SCHED_TASKS=120000
ZCO_PERF_YIELD_INTERVAL=8
ZCO_PERF_STACK_SIZE=65536
ZCO_PERF_SHARED_STACK_NUM=64
ZCO_PERF_CHANNEL_MESSAGES=80000
ZCO_PERF_TIMER_TASKS=60000
ZCO_PERF_HOOK_ROUNDS=40000
```

脚本入口：

```bash
BIN=build/perf/zco/tests/zco_performance zco/tests/benchmark/zco_perf.sh baseline
BIN=build/perf/zco/tests/zco_performance zco/tests/benchmark/zco_perf.sh perf
BIN=build/perf/zco/tests/zco_performance zco/tests/benchmark/zco_perf.sh valgrind
```

性能测试建议使用 `RelWithDebInfo` 和 frame pointer，仓库 `perf` preset 已经覆盖这点。
结果会受 CPU、内核、调度器数量、栈大小、系统负载和 allocator 策略影响，应在同一
机器和同一参数下比较。

## 支持功能

- 多调度器协程运行时
- `go()` 投递普通函数对象、`Task`、`Closure*` 和带参数调用
- 独立栈和共享栈两种协程栈模型
- 可配置协程栈大小和共享栈数量
- 协程句柄注册、恢复和安全清理
- work stealing 调度队列
- 协程友好的 `Event`、`Mutex`、`WaitGroup`
- 有界 `Channel<T>`，支持阻塞读写、超时读写、try 读写和 close
- 定时器、epoll poller、I/O event
- 系统调用 hook 与超时管理
- 测试专用 `zco_stdalloc` 目标，避免 allocator override 干扰运行时测试

