# zcoroutine 性能测试与优化报告

## 1. 目标与范围

本次工作按两个方向对 `zcoroutine` 做性能优化，并输出可复现实验结果：

1. 使用 `perf record/report` 定位 CPU 热点并做热点优化。
2. 使用 `valgrind --tool=cachegrind` 分析缓存与分支行为，并针对缓存友好性做代码优化。

另外按补充要求，对性能测试入口做了两项调整：

1. 压测入口改为独立 benchmark，可直接运行，不再依赖 gtest。
2. benchmark 启动时显式关闭 `zcoroutine` 日志，避免日志开销污染结果。

本次 workload 使用 `zcoroutine/tests/stress/stack_model_perf.cc`。

## 2. 环境与构建

- 日期：2026-04-10
- 工作目录：`/home/betty/repositories/zlynx/.worktrees/zcoroutine-perf`
- 构建目录：`build-zcoroutine-perf-nooverride`
- 构建类型：`RelWithDebInfo`
- 关键 CMake 选项：
  - `-DZCOROUTINE_BUILD_TESTS=ON`
  - `-DZLYNX_USE_ZMALLOC_OVERRIDE=OFF`

构建命令：

```bash
cmake -S . -B build-zcoroutine-perf-nooverride \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DZCOROUTINE_BUILD_TESTS=ON \
  -DZLYNX_USE_ZMALLOC_OVERRIDE=OFF
cmake --build build-zcoroutine-perf-nooverride --target stack_model_perf -j
```

说明：

- 关闭 `zmalloc override` 是为规避 profiling 包裹时的退出期崩溃。
- benchmark 入口在程序启动时调用 `disable_benchmark_logging()`，日志级别设为 `OFF`。
- `timer_sleep` 场景内部使用的是 `co_sleep_for(0)`，本质上是 zero-sleep yield 快路径，对调度公平性较敏感，不等同于真实毫秒级 timer latency。

## 3. 测试命令

基线：

```bash
BUILD_DIR=/home/betty/repositories/zlynx/.worktrees/zcoroutine-perf/build-zcoroutine-perf-nooverride \
bash zcoroutine/tests/stress/run_stack_model_perf_profile.sh baseline
```

热点采样：

```bash
BUILD_DIR=/home/betty/repositories/zlynx/.worktrees/zcoroutine-perf/build-zcoroutine-perf-nooverride \
bash zcoroutine/tests/stress/run_stack_model_perf_profile.sh perf
```

缓存分析：

```bash
BUILD_DIR=/home/betty/repositories/zlynx/.worktrees/zcoroutine-perf/build-zcoroutine-perf-nooverride \
ZCOROUTINE_PERF_SCALE_PCT=5 \
bash zcoroutine/tests/stress/run_stack_model_perf_profile.sh valgrind
```

最终采用的主要 artifact：

- baseline: `zcoroutine/tests/stress/perf_results/baseline_20260410_013102`
- perf: `zcoroutine/tests/stress/perf_results/perf_20260410_013102`
- valgrind: `zcoroutine/tests/stress/perf_results/valgrind_20260410_013102`

优化前对照组：

- baseline: `zcoroutine/tests/stress/perf_results/baseline_20260410_005510`
- perf: `zcoroutine/tests/stress/perf_results/perf_20260410_005510`
- valgrind: `zcoroutine/tests/stress/perf_results/valgrind_20260410_005510`

## 4. 代码改动摘要

### 4.1 性能测试改造

- `zcoroutine/tests/stress/stack_model_perf.cc`
  - 从 gtest 压测改成独立 `main()` benchmark。
  - 增加参数读取、结果打印和失败退出。
  - benchmark 启动时关闭日志。
- `zcoroutine/tests/CMakeLists.txt`
  - 增加 benchmark 目标，保留 smoke 级 ctest。
- `zcoroutine/tests/stress/run_stack_model_perf_profile.sh`
  - 改为直接运行 benchmark 二进制。
  - 支持显式 `BUILD_DIR` 和 `ZCOROUTINE_PERF_SCALE_PCT`。

### 4.2 perf 热点优化

- 惰性协程句柄注册：
  - `Runtime::register_fiber()` 不再在任务 materialize 时无条件执行。
  - 只有 `current_coroutine()` 需要对外句柄时才注册映射。
- 日志快路径：
  - 在 `zcoroutine/log.h` 中增加缓存日志级别。
  - 日志宏先走 `should_log()`，避免热路径反复进入 `get_logger()`。
- `StealQueue::size()` 改为原子快照：
  - 读负载统计时避免每次拿互斥锁。

### 4.3 cache / 容器访问优化

- `StealQueue::append()`、`Processor::enqueue_ready_batch()` 改为 move-iterator 批量搬运，减少逐项 push/pop。
- `Processor::drain_new_tasks()` 改为固定小批次 materialize，避免一次性把全部 `Task` 膨胀成 `Fiber`，降低热数据集尺寸。
- `run_ready_tasks()` 保留批量 drain ready queue 的实现，减少 ready 队列锁的高频往返。

## 5. 基线结果

优化前后对比，单位均为 `ops/s`。

| 场景 | 优化前 | 优化后 | 变化 |
|---|---:|---:|---:|
| scheduler_submit/shared | 464,516.12 | 4,169,474.26 | +797.6% |
| scheduler_submit/independent | 217,318.85 | 4,060,154.16 | +1768.3% |
| channel_spsc/shared | 2,259,622.49 | 1,942,938.24 | -14.0% |
| timer_sleep/shared | 593,658.53 | 1,104,573.81 | +86.1% |
| hook_socketpair/shared | 841,471.96 | 830,507.43 | -1.3% |

结论：

- 最核心的提交/调度路径提升最明显，`scheduler_submit` 两个场景都有数量级改善。
- `timer_sleep` 基准也有明显提升。
- `channel_spsc` 与 `hook_socketpair` 没有同步获益，其中 `channel_spsc` 有小幅回退。

直接运行 benchmark 的一次 sanity 结果：

```text
scheduler_submit/shared      4,477,293.52 ops/s
scheduler_submit/independent 4,291,535.91 ops/s
channel_spsc/shared          2,012,704.70 ops/s
timer_sleep/shared             308,722.11 ops/s
hook_socketpair/shared       1,101,336.75 ops/s
```

## 6. perf 热点变化

优化前 `perf report` 的主要热点：

- `Processor::run_ready_tasks()` 55.06%
- `Processor::drain_new_tasks()` 25.84%
- `Runtime::unregister_fiber()` / `FiberHandleRegistry::unregister_fiber()` 22.47%
- `Runtime::register_fiber()` / `FiberHandleRegistry::register_fiber()` 12.36% / 10.11%
- `get_logger()` 2.25%

优化后 `perf report` 的主要热点：

- `Processor::drain_new_tasks()` 29.41%
- `Processor::obtain_fiber()` 17.65%
- `Processor::run_ready_tasks()` 23.53%

关键变化：

- `register_fiber` / `unregister_fiber` 已不再是 top hotspot。
- `get_logger()` 已从热点中消失。
- `StealQueue::size()` 在 perf 中仅剩很小占比。
- 热点重新收敛到真正的调度核心：
  - `drain_new_tasks`
  - `obtain_fiber`
  - `run_ready_tasks`

这说明第一类优化是有效的：原来由句柄注册和日志带来的额外热开销已经被显著压缩。

## 7. cachegrind 结果

对比 `valgrind_20260410_005510` 与 `valgrind_20260410_013102`：

| 指标 | 优化前 | 优化后 | 变化 |
|---|---:|---:|---:|
| D refs | 41,432,717 | 29,348,921 | -29.2% |
| D1 misses | 816,484 | 768,003 | -5.9% |
| LLd misses | 177,623 | 134,263 | -24.4% |
| Branches | 14,458,054 | 10,358,177 | -28.4% |
| Branch mispredicts | 333,112 | 218,671 | -34.4% |
| D1 miss rate | 1.97% | 2.62% | +32.8% |
| LLd miss rate | 0.43% | 0.46% | +6.7% |
| Branch mispredict rate | 2.30% | 2.11% | -8.4% |

解读：

- 绝对访问量显著下降，说明热路径做了更少的数据访问。
- `D1 misses` 和 `LLd misses` 的绝对值都下降了，其中 `LLd misses` 下降 24.4%。
- 分支预测也有改进，`Branch mispredicts` 下降 34.4%。
- 但 miss rate 没有全面改善：
  - `D1 miss rate` 上升
  - `LLd miss rate` 小幅上升

因此第二类优化的结论必须如实写明：

- 从“绝对 miss 数”和“分支失误数”看，缓存/控制流行为更轻了。
- 从“命中率/失效率比例”看，并不是全面改善。
- 这是本轮优化后的剩余问题，后续仍应继续压缩 `Fiber` 构造与 ready/run queue 的数据布局成本。

从 `cachegrind` 的 file:function 视图看，`get_logger()`、`Runtime::register_fiber()` 已不再是主要热项；`StealQueue::size()` 占比也明显缩小，主要压力转移到了：

- `Processor::run_ready_tasks()`
- `Processor::obtain_fiber()`
- `Processor::drain_new_tasks()`

## 8. 验证结果

执行过的验证命令：

```bash
ctest --test-dir build-zcoroutine-perf-nooverride -L quick --output-on-failure
ctest --test-dir build-zcoroutine-perf-nooverride -R '^(runtime_stress|stack_model_perf_smoke)$' --output-on-failure
build-zcoroutine-perf-nooverride/zcoroutine/tests/stack_model_perf
```

结果：

- `quick`：28/28 通过
- `runtime_stress`：通过
- `stack_model_perf_smoke`：通过
- benchmark 可直接独立运行，不依赖 gtest

## 9. 结论

本次优化已经完成两个方向的目标：

1. `perf` 方向：
   - 去掉了句柄注册和日志访问对热路径的明显污染。
   - `scheduler_submit` 吞吐获得数量级提升。
2. `cachegrind` 方向：
   - 降低了总数据访问量、绝对 cache miss 数和分支失误数。
   - 但 miss rate 没有全面改善，这部分仍有继续优化空间。

当前最值得继续追的热点已经比较明确：

1. `Processor::drain_new_tasks()`
2. `Processor::obtain_fiber()`
3. `Processor::run_ready_tasks()`

后续建议：

1. 继续降低 `Fiber` materialize/reset 的分配与初始化成本。
2. 重新审视 `run_queue_` / `steal_queue_` 的容器布局，进一步降低写放大。
3. 如果继续做 cache 优化，优先围绕 `Fiber` 对象布局和 ready queue 数据结构做，而不是继续在日志或句柄注册上花时间。
