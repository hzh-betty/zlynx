# zmalloc

zmalloc 提供显式 `zmalloc()` / `zfree()` 接口，以及可选的全局
`malloc/free/new/delete` 替换目标。生产库最低使用 C++11，依赖 Threads。

## 最小示例

```cpp
#include "zmalloc/zmalloc.h"
#include <cstring>

int main() {
    void *memory = zmalloc::zmalloc(1024);
    std::memset(memory, 0, 1024);
    zmalloc::zfree(memory);
    return 0;
}
```

`zmalloc(0)` 返回空指针，分配失败抛出 `std::bad_alloc`；`zfree(nullptr)` 无操作。
显式 `zfree()` 的非空参数必须是本分配器返回且尚未释放的指针。

## 分配路径

```text
小对象 → ThreadCache → TransferCache → CentralCache → PageCache → 系统页分配
大对象 → PageCache → 系统页分配
释放   → PageMap 定位 Span → 对象缓存或页缓存
```

小对象上限为 256 KiB，分配器页粒度为 8 KiB。线程缓存处理小对象热路径，
TransferCache 批量转移对象，CentralCache 管理各大小类的 Span，PageCache
处理页块分配、切分、合并及复用。

`memory_stats()` 返回当前线程缓存、共享缓存和页映射的统计；各层分别取样，
整体不是原子快照，也不等同于进程 RSS。

`release_memory()` 清理调用线程缓存、有界排空共享传输缓存，并对完全空闲页
调用 `MADV_DONTNEED`。它保留虚拟地址映射，不清理其他线程的本地缓存，返回
本次成功建议回收的字节数。构建选项 `ZMALLOC_AUTO_RELEASE=ON` 可启用有界自动
页回收，默认关闭；显式回收接口始终可用。

## 构建与测试

以下命令从 zlynx 根目录执行，需要 CMake 3.18+。默认开启测试，测试需要
GTest 和支持 C++17 的编译器：

```bash
cmake -S zmalloc -B build/zmalloc -DCMAKE_BUILD_TYPE=Debug
cmake --build build/zmalloc -j 2
(cd build/zmalloc && ctest --output-on-failure)
```

仅验证 C++11 生产库：

```bash
cmake -S zmalloc -B build/zmalloc-cxx11 \
  -DCMAKE_CXX_STANDARD=11 -DBUILD_TESTING=OFF
cmake --build build/zmalloc-cxx11 -j 2
```

整体构建与预设用法见[根 README](../README.md)。整体构建后可用
`ctest -R '^zmalloc_'` 筛选本模块测试，测试覆盖缓存、页映射、显式分配、
全局替换及并发分配路径。

## 安装与使用

```bash
cmake --install build/zmalloc --prefix /path/to/prefix
```

消费者配置时传入 `-DCMAKE_PREFIX_PATH=/path/to/prefix`：

```cmake
find_package(zmalloc 1 CONFIG REQUIRED)
add_executable(app main.cc)
target_link_libraries(app PRIVATE zmalloc::zmalloc)
```

全局替换由最终程序按需链接：

```cmake
target_link_libraries(app PRIVATE zmalloc::override)
```

`zmalloc::override` 是静态目标，并传递核心分配器依赖。替换入口按 libc 约定处理
失败；普通 `new` 抛出 `std::bad_alloc`，`nothrow new` 返回空指针。
Linux 下构建使用 `initial-exec` TLS，应随进程启动链接，运行后的动态加载未作保证。
公共头文件包含内联实现和内部类型，更新库时应同步重新编译消费者。

## 性能与源码

从仓库根目录构建性能目标：

```bash
cmake --preset relwithdebinfo -DBUILD_BENCHMARKS=ON
cmake --build --preset relwithdebinfo \
  --target zmalloc_benchmark zmalloc_performance thread_cache_benchmark -j 2
build/relwithdebinfo/zmalloc/tests/zmalloc_benchmark
```

`zmalloc_benchmark` 对比显式分配器与系统分配器，`zmalloc_performance` 提供
压测驱动，`thread_cache_benchmark` 聚焦线程缓存。源码位于
[tests/benchmark](tests/benchmark)。覆盖率入口见[根 README](../README.md)。

公共入口为 [zmalloc.h](include/zmalloc/zmalloc.h)，缓存和 Span 定义位于
[include/zmalloc/internal](include/zmalloc/internal)，实现位于 [src](src)，
全局替换入口为 [override.cc](src/override.cc)。
