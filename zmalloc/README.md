# zmalloc

`zmalloc` 是 zlynx 的高性能内存分配模块。它提供显式 `zmalloc()` / `zfree()`
接口，也提供可选的 `zmalloc_override` 目标用于全局替换 `malloc/free/new/delete`。
模块面向大量小对象、多线程分配和网络运行时热路径。

## 快速开始

显式使用：

```cpp
#include "zmalloc/zmalloc.h"

#include <cstring>

int main() {
    void *p = zmalloc::zmalloc(1024);
    std::memset(p, 0, 1024);
    zmalloc::zfree(p);
    return 0;
}
```

安装后消费：

```cmake
cmake_minimum_required(VERSION 3.18)
project(zmalloc_demo LANGUAGES CXX)

find_package(zmalloc CONFIG REQUIRED)

add_executable(zmalloc_demo main.cc)
target_link_libraries(zmalloc_demo PRIVATE zmalloc::zmalloc)
```

如果需要全局替换分配器，链接 `zmalloc::override`：

```cmake
target_link_libraries(zmalloc_demo PRIVATE zmalloc::override)
```

在 zlynx 源码树整体构建时，`ZLYNX_USE_ZMALLOC_OVERRIDE` 默认开启，根工程会把
`zmalloc_override` 私有链接到 `zlog`、`zco`、`znet`、`zhttp` 等运行时模块。

## 项目架构

`zmalloc` 的小对象路径按线程缓存、中心缓存、页缓存分层：

```text
zmalloc()
  -> ThreadCache       线程本地自由链表，热路径无锁
  -> TransferCache     线程缓存与中心缓存之间的批量转移层
  -> CentralCache      按 size class 管理 span 内对象
  -> PageCache         管理页级 span，负责合并/切分
  -> SystemAlloc       向操作系统申请页

zfree()
  -> 根据 PageMap 找到 Span
  -> 小对象回到 ThreadCache/CentralCache
  -> 大对象回到 PageCache
```

核心目录：

```text
zmalloc/
  include/zmalloc/zmalloc.h        对外统一接口
  include/zmalloc/internal/        size class、cache、span、page map 等内部结构
  src/                             分配器实现
  src/override.cc                  全局 malloc/free/new/delete 替换符号
  tests/unit/                      单元测试
  tests/integration/               并发分配集成测试
  tests/benchmark/                 benchmark 和 perf driver
```

主要组件：

- `SizeClass`：请求大小到对齐尺寸、大小类索引、批量移动数量和页数的映射。
- `ThreadCache`：每线程缓存，处理小对象分配/释放热路径。
- `TransferCache`：降低 thread cache 和 central cache 之间的竞争。
- `CentralCache`：按 size class 管理 span 和对象自由链表。
- `PageCache`：页级 span 分配、释放、合并与对象到 span 映射。
- `SpanList` / `FreeList` / `PageMap` / `ObjectPool`：分配器基础结构。
- `SystemAlloc`：向系统申请内存页。
- `override`：可选全局替换符号。

## 依赖

基础构建依赖：

- CMake 3.18+
- C++14 编译器，仓库 preset 默认使用 `clang++`
- Ninja，使用 preset 时需要
- Threads

测试和分析额外依赖：

- GTest / GMock。`zmalloc/tests/CMakeLists.txt` 使用 `find_package(GTest QUIET)`，
  找不到时会跳过 zmalloc 测试。
- `gcovr`
- `perf`、`valgrind`、`callgrind_annotate`，可选性能分析工具

## 编译

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
cmake --build --preset perf --target zmalloc_benchmark
cmake --build --preset perf --target zmalloc_performance
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

安装后导出：

- `zmalloc::zmalloc`：显式分配器接口。
- `zmalloc::override`：全局替换符号，按需链接。

## 测试

运行全部 zmalloc 测试：

```bash
cmake --build --preset debug --target zmalloc_test
```

只跑单元测试：

```bash
cmake --build --preset debug --target zmalloc_test_unit
```

只跑集成测试：

```bash
cmake --build --preset debug --target zmalloc_test_integration
```

直接使用 CTest：

```bash
ctest --test-dir build/debug -R '^zmalloc\.' --output-on-failure
ctest --test-dir build/debug -R '^zmalloc\.unit\.' --output-on-failure
ctest --test-dir build/debug -R '^zmalloc\.integration\.' --output-on-failure
```

当前测试覆盖的主要行为：

- size class 对齐、索引、预计算查找表
- free list、object pool、span list
- page map、page cache、central cache、transfer cache、thread cache
- system alloc
- `zmalloc()` / `zfree()` 小对象和大对象路径
- `zmalloc_override` 白盒测试
- allocator override 行为
- 并发分配集成测试

## 覆盖率

统一脚本：

```bash
coverage/run_coverage.sh
```

`coverage/zmalloc-summary.txt` 中记录的当前 zmalloc 覆盖率：

| 指标 | 覆盖率 |
|---|---:|
| Lines | 97.5% (826 / 847) |
| Functions | 100.0% (98 / 98) |
| Branches | 90.2% (333 / 369) |
| Decisions | 97.1% (169 / 174) |

覆盖率报告统计 `zmalloc/src`，包含 `override.cc`。

## 性能

`zmalloc_benchmark` 对比 `zmalloc` 和系统 `malloc/free`，覆盖固定大小、随机大小、
单线程和多线程场景。

```bash
cmake --preset perf
cmake --build --preset perf --target zmalloc_benchmark

build/perf/zmalloc/tests/zmalloc_benchmark
```

`zmalloc_performance` 只测试 `zmalloc/zfree` 路径，适合 perf/callgrind 分析：

```bash
cmake --build --preset perf --target zmalloc_performance

build/perf/zmalloc/tests/zmalloc_performance \
  --threads 8 \
  --min-size 1 \
  --max-size 8192 \
  --allocs 200000 \
  --rounds 20 \
  --touch
```

常用参数：

```bash
-t, --threads N
-s, --size BYTES
--min-size BYTES
--max-size BYTES
-n, --allocs N
-r, --rounds N
--touch
```

性能测试建议使用 `RelWithDebInfo`。分配器结果对对象大小分布、线程数、是否触碰内存、
NUMA、CPU cache、系统 malloc 实现和是否启用 `zmalloc_override` 都很敏感。

## 支持功能

- 显式 `zmalloc(size)` / `zfree(ptr)`
- 小对象按 size class 分配，当前小对象上限为 `MAX_BYTES = 256 KiB`
- 8 KiB 页粒度，`PAGE_SHIFT = 13`
- 线程本地缓存热路径
- TransferCache 批量转移，降低中心缓存竞争
- CentralCache span 管理
- PageCache 页级 span 申请、释放与合并
- PageMap 对象到 span 映射
- 大对象页级分配
- 可选全局 `malloc/free/new/delete` 替换目标
- 多线程并发分配测试和性能 driver

