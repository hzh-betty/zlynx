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

全局替换入口按 libc 约定处理分配失败，返回空指针并设置 `ENOMEM`；普通
`new` 抛出 `std::bad_alloc`，`nothrow new` 返回空指针。`malloc(0)` 返回可释放的
指针，显式 `zmalloc(0)` 仍返回空指针。替换目标也提供对齐分配和
`malloc_usable_size`，在 glibc 下支持释放、调整和查询外部 glibc 分配块。

Linux 构建使用 `initial-exec` TLS，模块及替换目标应随进程启动链接；运行后的
动态加载场景未作保证。内联接口和 PageCache 布局参与调用方编译，升级本模块
时应同步重新编译消费目标，避免旧目标与新版共享库混用。共享库 ABI 已升级
为 2，旧的 `libzmalloc.so.1` 调用方需重新链接到 `libzmalloc.so.2`。

PageCache 对超过 1 MiB、不超过 8 MiB 的空闲大块按精确页数复用，总缓存预算
为 16 MiB，超预算驱逐最早归还的块；超过 8 MiB 的块释放时直接解除映射。

自动回收默认关闭，可在构建时指定 `-DZLYNX_ZMALLOC_AUTO_RELEASE=ON` 启用；
显式 `release_memory()` 始终可用。启用后，完全空闲且尚未建议回收的页达到
32 MiB 时，每累计归还 8 MiB 页触发一次
自动 `MADV_DONTNEED`，单轮最多检查 32 个 Span、建议回收 8 MiB。触发点在
页归还路径，没有后台线程；停止分配/释放后不会继续回收。自动回收保留虚拟
映射，不清理其他线程的缓存，后续重新触页可能产生缺页开销。

批量释放后，可显式归还完全空闲页的物理内存：

```cpp
const size_t advised_bytes = zmalloc::release_memory();
const zmalloc::MemoryStats stats = zmalloc::memory_stats();
```

`release_memory()` 清理调用线程的空闲对象缓存，按各类初始数量有界排空
TransferCache，再对完全空闲的 Span 调用 `MADV_DONTNEED`。它保留虚拟地址，
可重复调用，后续分配继续复用这些页。并发分配时，共享缓存不保证最终为空；
其他线程的本地缓存需要由各线程自行清理。回收期间持有页锁，可能短暂阻塞页级分配。

`MemoryStats` 提供当前线程/共享传输缓存字节数、空闲页字节数、其中整段已建议
回收的页字节数、受管页映射字节数和累计建议回收字节数。统计不含元数据，
各层取样不是整体原子快照；建议回收字节数不等于 RSS 降幅，也不会减少虚拟地址占用。

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
- C++17 编译器，仓库 preset 默认使用 `clang++`
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

