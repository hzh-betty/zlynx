# ThreadCache 策略与基准验证

线程缓存默认启用，公共 API 与内部 API 共用内联入口；TransferCache 暂不接入。
每个规格从 1 个对象慢启动，达到一批后按批增长，最多四批；释放持续超限时缩容。
每线程设置 1 MiB **软预算**，超限扫描低水位并归还闲置对象。这个预算不是硬上限，
完全闲置的线程不会自动触发扫描。线程退出时关闭缓存并以每批最多 128 个对象归还
CentralCache；较晚的 TLS / pthread / 静态析构直接走 CentralCache。
CentralCache 和 PageCache 在静态存储上构造，不注册析构，服务于整个进程生命周期。
PageCache 可以继续保留已归还的页，因此对象回收不等于 RSS 立即下降。

## 运行

```sh
cmake --preset perf
cmake --build --preset perf --target zmalloc_thread_cache_benchmark
build/perf/zmalloc/tests/zmalloc_thread_cache_benchmark single 64
build/perf/zmalloc/tests/zmalloc_thread_cache_benchmark cross 64
build/perf/zmalloc/tests/zmalloc_thread_cache_benchmark churn 4096
```

`single`：单线程每批分配/释放 256 个对象，共 40000 批。
`cross`：两个常驻线程通过 256 槽 SPSC 队列交接，同样分配/释放 10240000 个对象。
`churn`：依次创建并 join 4000 个线程，每个线程分配/释放 256 个对象。
每次分配写入首字节，避免只测虚拟地址预留；队列同步、线程创建和退出时间计入结果。
RSS 从 Linux `/proc/self/status` 读取；峰值针对当前进程地址空间。

## 本次结果

2026-09-16，x86-64 虚拟化环境，CPU 报告为 i5-12500H，Clang 21.1.8 / GCC 15 标准库。
基线为 `2bcdf50`，与修改版使用同一份驱动，均将除 `override.cc` 外的 allocator 源文件
直接编译进二进制，选项 `-std=c++14 -O3 -DNDEBUG -pthread`。未启用全局 malloc 替换。
每种大小、场景交替运行修改前后各三次，以下取中位数；未绑核，结果包含调度噪声，
用于确认方向，不代表真实业务或所有对象大小的性能。

吞吐单位为百万次“分配 + 释放”/秒，RSS 为运行结束时的 MiB。

| 大小 | 场景 | 修改前吞吐 | 修改后吞吐 | 修改前 RSS | 修改后 RSS |
|---|---|---:|---:|---:|---:|
| 64 B | single | 50.32 | 166.51 | 4.33 | 4.35 |
| 64 B | cross | 9.40 | 29.38 | 4.64 | 4.65 |
| 64 B | churn | 4.85 | 5.06 | 5.12 | 4.64 |
| 4096 B | single | 12.51 | 14.23 | 5.34 | 5.36 |
| 4096 B | cross | 6.68 | 6.79 | 5.63 | 5.68 |
| 4096 B | churn | 3.52 | 3.42 | 37.13 | 5.63 |

64 B 场景收益明显；4 KiB 的变化较小，短命线程存在吞吐波动，但驻留内存明显下降。
1 MiB 预算和四批容量是初始策略，尚未做跨工作负载参数寻优。

### 锁等待抽样

另在两份临时源码副本中，对 `SpinLock::lock()` 的竞争慢路径加入计时与计数，
运行相同的 cross 场景各三次。正式代码没有加入计时器或统计原子变量。
下表是竞争次数和累计等待时间的中位数；累计时间可重叠，并包括等待期间的调度时间。
计时和统计本身会扰动竞争，不能与上表未插桩吞吐直接混用，也不包含 PageCache 的 mutex 等待。

| 大小 | 修改前竞争次数 | 修改后竞争次数 | 修改前累计等待 ms | 修改后累计等待 ms |
|---|---:|---:|---:|---:|
| 64 B | 2900289 | 155374 | 759.10 | 142.09 |
| 4096 B | 3336636 | 3988099 | 998.09 | 1016.37 |

批量策略显著减少了 64 B 场景的锁等待，4 KiB 场景仍存在竞争，不能据此认定已解决所有
规格的锁瓶颈。TransferCache 的收益和额外内存占用留待单独评估。

## 正确性检查

- Debug / Release 构建，全项目 90 项 CTest。
- ThreadCache 覆盖对齐字节记账、批量清理、低水位、预算触发、容量缩减、重复关闭。
- 反复创建/退出线程，验证存活对象所属 Span 的 use_count 仅保留真实使用者。
- 关闭后继续 malloc/free、较晚执行的 C++ TLS 和 pthread 析构，以及主线程静态退出阶段。
- ThreadCache 79 个用例和并发分配 50 个用例使用 AddressSanitizer / UndefinedBehaviorSanitizer 检查；
  Sanitizer 检查不链接全局 allocator override，以避免替换 Sanitizer 自身的分配器。
