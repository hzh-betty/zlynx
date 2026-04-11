# znet 性能测试与优化报告

## 1. 目标与范围

本次工作围绕 `znet` 做两类性能分析与优化，并输出可复现实验结果：

1. 使用 `perf record/report` 定位 CPU 热点函数，并针对热点路径做优化。
2. 使用 `valgrind --tool=cachegrind` 分析缓存与分支行为，优化代码访问模式，尽量降低缓存 miss。

压测入口使用 `znet/tests/benchmark/znet_wrk_benchmark.cc`，由内置 echo server 配合 `wrk` 产生负载。

## 2. 环境与构建

- 日期：2026-04-10
- 主工作目录：`/home/betty/repositories/zlynx`
- cachegrind 对照组工作目录：`/home/betty/repositories/zlynx/.worktrees/znet-perf-baseline-worktree`
- 构建目录：`build-znet-perf`
- 构建类型：`RelWithDebInfo`
- 关键 CMake 选项：
  - `-DENABLE_TESTS=ON`
  - `-DZLYNX_USE_ZMALLOC_OVERRIDE=OFF`

构建命令：

```bash
cmake -S . -B build-znet-perf -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_TESTS=ON \
  -DZLYNX_USE_ZMALLOC_OVERRIDE=OFF

cmake --build build-znet-perf -j --target \
  znet_wrk_benchmark \
  tcp_connection_unit \
  buffer_unit \
  socket_unit \
  tcp_server_unit \
  server_unit \
  tls_context_unit
```

说明：

- `cachegrind` 跑完整压测代价过高，因此脚本默认使用 `ZNET_PERF_SCALE_PCT=25` 做缩放压测。
- 为拿到真正的 server 侧缓存数据，本次修正了 `run_znet_wrk_perf.sh`，启用 `--trace-children=yes`，并自动选择最大的 `cachegrind.out.*` 子进程结果，而不是只分析 benchmark 父进程。

## 3. 测试命令与 Artifacts

基线吞吐：

```bash
BUILD_DIR=/home/betty/repositories/zlynx/build-znet-perf \
bash znet/tests/benchmark/run_znet_wrk_perf.sh baseline
```

热点采样：

```bash
BUILD_DIR=/home/betty/repositories/zlynx/build-znet-perf \
bash znet/tests/benchmark/run_znet_wrk_perf.sh perf
```

缓存分析：

```bash
BUILD_DIR=/home/betty/repositories/zlynx/build-znet-perf \
bash znet/tests/benchmark/run_znet_wrk_perf.sh valgrind
```

本报告采用的主要 artifact：

- 优化前 baseline：`znet/tests/benchmark/perf_results/baseline_20260410_090112`
- 优化后 baseline：`znet/tests/benchmark/perf_results/baseline_20260410_090854`
- 优化前 perf：`znet/tests/benchmark/perf_results/perf_20260410_090124`
- 优化后 perf：`znet/tests/benchmark/perf_results/perf_20260410_090905`
- 优化前 cachegrind：`.worktrees/znet-perf-baseline-worktree/znet/tests/benchmark/perf_results/valgrind_20260410_091029`
- 优化后 cachegrind：`znet/tests/benchmark/perf_results/valgrind_20260410_090905`

## 4. 代码改动摘要

### 4.1 perf 热点优化

- `znet/src/tcp_connection.cc`
  - 为 `read/send/flush_output/shutdown/close` 增加 inline actor fast path。
  - 当调用方已经在归属 scheduler 的协程上下文内时，直接执行内部逻辑，绕过 `Event` 分配、mailbox 入队和 `wait()`。
- `znet/src/tcp_connection.cc`
  - `send_internal()` 改为 `const char* + size_t` 接口，避免热路径构造临时 `std::string`。
  - 在 `output_buffer_` 为空且非 TLS 场景下，优先直接 `send()` 到 socket，只把未发送尾部写入缓冲区。
- `znet/src/buffer.cc`
  - `Buffer::make_space()` 中将重叠区搬移改为 `std::memmove()`，减少不必要的逐元素拷贝路径，并修正重叠拷贝语义。

### 4.2 cache / 访存行为优化

- `znet/src/tcp_connection.cc`
  - 通过 inline fast path 减少 `shared_ptr<Event>` 生命周期、条件等待和 mailbox 操作，直接压缩热数据访问量。
  - 通过 direct send 降低 `std::string::_M_mutate`、`memcpy/memmove` 和输出缓冲写入压力。
- `znet/tests/benchmark/run_znet_wrk_perf.sh`
  - 修正 `cachegrind` 的采样方式，确保分析的确实是 server 进程缓存行为。

### 4.3 回归测试补充

- `znet/tests/buffer_unit.cc`
  - 增加 `ReusesPrependSpaceWithoutCorruptingReadableData`。
- `znet/tests/tcp_connection_unit.cc`
  - 增加 `SendSucceedsInsideCoroutineContext`。

## 5. 基线吞吐结果

| 指标 | 优化前 | 优化后 | 变化 |
|---|---:|---:|---:|
| Requests/sec | 111,296.92 | 116,053.29 | +4.3% |
| Transfer/sec | 13.16 MB | 13.72 MB | +4.3% |

说明：

- 吞吐提升幅度不算激进，但在热点函数明显降温的同时，最终 QPS 有稳定正收益。
- 当前代码的再次冒烟运行结果为 `116,168.43 req/s`，与优化后 artifact 基本一致。

## 6. perf 热点变化

优化前 `perf report` 关键热点：

| 热点 | 优化前占比 | 优化后占比 | 变化 |
|---|---:|---:|---:|
| `znet::TcpConnection::drain_mailbox()` | 2.71% | 1.66% | -38.7% |
| `znet::TcpConnection::dispatch_event_and_wait(...)` | 1.36% | 0.60% | -55.9% |
| `zco::Event::wait(unsigned int) const` | 1.90% | 0.15% | -92.1% |
| `znet::TcpConnection::read(...)` | 1.63% | 1.05% | -35.6% |
| `__memmove_avx_unaligned_erms` | 5.42% | 4.22% | -22.1% |

新增但可接受的热点：

- `znet::TcpConnection::send_internal(char const*, unsigned long, unsigned int)`：0.60%
- `znet::TcpConnection::try_begin_inline_actor()`：0.45%

结论：

- 第一类优化是明确生效的。
- 原先由 mailbox 派发、事件等待和跨协程同步带来的热开销，已明显收敛。
- 热点更集中在真正的数据发送与连接处理路径，而不是调度包装成本。

## 7. cachegrind 结果

### 7.1 总体指标

| 指标 | 优化前 | 优化后 | 变化 |
|---|---:|---:|---:|
| Ir | 401,270,839 | 243,937,036 | -39.2% |
| Dr | 116,570,801 | 70,175,465 | -39.8% |
| Dw | 65,270,547 | 39,236,462 | -39.9% |
| D1 misses | 3,728,896 | 2,659,175 | -28.7% |
| LLd misses | 47,315 | 47,434 | +0.3% |
| Branch mispredicts | 3,459,426 | 1,956,554 | -43.4% |
| D1 miss rate | 2.05% | 2.43% | +18.5% |
| D1 hit rate | 97.95% | 97.57% | -0.38 pct |
| Branch mispredict rate | 5.69% | 5.19% | -0.50 pct |

### 7.2 函数级对比

优化前较重的缓存/访存热点：

- `znet::TcpConnection::dispatch_event_and_wait(...)`：Ir 3,328,030
- `znet::TcpConnection::drain_mailbox()`：Ir 2,403,609
- `znet::TcpConnection::send_internal(std::string const&, ...)`：Ir 1,506,505
- `std::string::_M_mutate(...)`：Ir 2,059,915
- `__memcpy_avx_unaligned_erms`：Ir 17,148,492

优化后对应变化：

- `znet::TcpConnection::drain_mailbox()`：Ir 1,524,487
- `znet::TcpConnection::send_internal(char const*, ...)`：Ir 1,519,658
- `znet::TcpConnection::read(...)`：Ir 1,024,686
- `znet::Buffer::read_from_socket(...)`：Ir 1,051,075
- `__memcpy_avx_unaligned_erms`：Ir 11,581,560

解读：

- 绝对访存量下降很明显，说明这轮改动确实压缩了热路径上的对象管理、字符串变更和缓冲区写入成本。
- `drain_mailbox` 的指令量显著下降，而 `dispatch_event_and_wait` 在 perf 中明显降温、在优化后 cachegrind top 列表中也不再突出，说明 inline actor fast path 有效。
- `__memcpy_avx_unaligned_erms` 从 1714.8 万降到 1158.2 万，说明 direct send 路径减少了数据搬运。

但第二类优化需要如实说明：

- 从“绝对 miss 数”看，D1 miss 降了 28.7%，分支失误降了 43.4%，缓存行为更轻。
- 从“命中率”看，D1 hit rate 并没有提升，反而从 97.95% 下降到 97.57%；LLd miss 也基本没有下降。
- 因此本轮 cache 优化的结论应表述为：
  - 访存总量下降，缓存 miss 的绝对数量下降。
  - 但全局缓存命中率没有同步改善，后续仍需继续优化数据布局和分配行为。

## 8. 验证结果

执行过的验证命令：

```bash
ctest --test-dir build-znet-perf \
  -R '^(buffer_unit|socket_unit|server_unit|tcp_connection_unit|tcp_server_unit|tls_context_unit)$' \
  --output-on-failure

build-znet-perf/znet/tests/znet_wrk_benchmark
```

结果：

- 6/6 单元测试通过。
- benchmark 可独立运行。
- 当前代码再次冒烟压测结果：`116,168.43 req/s`，与优化后 artifact 一致。

## 9. 结论

本次 `znet` 性能优化有两点确定收益：

1. `perf` 方向优化有效，热点从 mailbox/event wait 等包装开销回收到了真正的连接处理路径，吞吐提升约 4.3%。
2. `cachegrind` 方向优化显著降低了指令数、数据读写次数、D1 miss 绝对数量和分支预测失败次数。

同时也必须保留一个明确结论：

- 本轮并没有把“全局缓存命中率”一起优化上去，D1 hit rate 略有下降。

后续如果继续做缓存命中率优化，优先建议看三类方向：

1. 减少 `shared_ptr`/堆分配密度，继续压低 `malloc/free` 热度。
2. 重新评估 `TcpConnection` mailbox 与 `Event` 对象的数据布局，尽量缩短热对象生命周期。
3. 继续压缩 `Buffer` 与字符串扩容路径上的拷贝和写放大。
