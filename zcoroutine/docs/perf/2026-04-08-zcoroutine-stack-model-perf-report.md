# zcoroutine 性能测试与优化报告（2026-04-08）

## 1. 目标

本次围绕 `stack_model_perf` 压测，从两个维度完成优化与验证：

1. 基于 `perf` 采样定位 CPU 热点并实施低风险优化。
2. 基于 `valgrind`（`cachegrind` + `callgrind`）分析缓存与调用热点，实施缓存友好改造。

## 2. 实施改动

### 2.1 调度器热路径优化（perf 维度）

1. `Processor::drain_new_tasks` 改为批量 ready 入队，减少每个 Fiber 的重复加锁/唤醒开销。
2. `sleep_for(0)` 增加 fast-path，直接 `yield_current()`，避免进入定时器队列。

涉及文件：

- `zcoroutine/src/processor.cc`
- `zcoroutine/include/zcoroutine/internal/processor.h`
- `zcoroutine/src/sched.cc`

### 2.2 缓存友好优化（valgrind 维度）

1. `Channel<T>` 内部存储由 `std::deque<T>` 改为连续内存 ring buffer（placement new + head/tail/size）。
2. 读写热路径减少重复 `closed_` 原子读取。
3. 增加 wrap-around FIFO 回归用例。

涉及文件：

- `zcoroutine/include/zcoroutine/channel.h`
- `zcoroutine/tests/unit/channel_unit.cc`

### 2.3 压测可调负载与 profiling 自动化

1. `stack_model_perf` 支持环境变量调整 workload（含 `ZCOROUTINE_PERF_SCALE_PCT`）。
2. 新增统一脚本：`baseline | perf | valgrind` 三模式。
3. 脚本支持临时调整 `kernel.perf_event_paranoid` 或切换 `sudo perf`。

涉及文件：

- `zcoroutine/tests/stress/stack_model_perf.cc`
- `zcoroutine/tests/stress/run_stack_model_perf_profile.sh`

## 3. 验证命令

### 3.1 构建与功能回归

```bash
cmake --build build --target channel_unit sched_unit stack_model_perf
ctest --test-dir build -R '^channel_unit$' --output-on-failure
ctest --test-dir build -R '^sched_unit$' --output-on-failure
ctest --test-dir build -R '^stack_model_perf$' --output-on-failure
```

执行结果：上述测试全部通过。

### 3.2 性能采样命令

```bash
./zcoroutine/tests/stress/run_stack_model_perf_profile.sh baseline
./zcoroutine/tests/stress/run_stack_model_perf_profile.sh perf
./zcoroutine/tests/stress/run_stack_model_perf_profile.sh valgrind
```

产物目录：

- `zcoroutine/tests/stress/perf_results/baseline_20260408_213528`
- `zcoroutine/tests/stress/perf_results/perf_20260408_213530`
- `zcoroutine/tests/stress/perf_results/valgrind_20260408_213533`

## 4. perf 结果

### 4.1 吞吐结果（baseline vs perf 运行）

| 场景 | baseline (ops/s) | perf-run (ops/s) | 变化 |
|---|---:|---:|---:|
| scheduler_submit(shared) | 273084.86 | 312442.66 | +14.41% |
| scheduler_submit(independent) | 143662.51 | 186502.93 | +29.82% |
| channel_spsc(shared) | 654274.75 | 787492.17 | +20.36% |
| timer_sleep(shared) | 304689.32 | 330819.36 | +8.58% |
| hook_socketpair(shared) | 1085549.14 | 994129.10 | -8.42% |

说明：`hook_socketpair` 在该轮存在抖动回落，属于后续需要重点复测的回归风险点。

### 4.2 采样热点（perf report 摘要）

主要热点集中在：

1. `zcoroutine::Processor::run_loop`
2. `zcoroutine::Processor::run_ready_tasks`
3. `zcoroutine::Processor::drain_new_tasks`
4. `zcoroutine::Processor::dispatch_resumed_fiber`
5. `zcoroutine::Runtime::register_fiber / unregister_fiber`

解读：调度器与 Fiber 生命周期管理仍是主热路径，批量入队已减少 `enqueue_ready` 的频繁出现，但 `register/unregister` 与 `shared_ptr` 路径仍占比较高。

### 4.3 PMU 限制

`perf stat` 输出为 `<not supported>`（`cycles/instructions/cache` 不可用），说明当前环境 PMU 受限。当前 CPU 侧结论主要依赖 `perf record` 调用图与吞吐数据。

## 5. valgrind 结果（cachegrind + callgrind）

### 5.1 workload 缩放配置

`valgrind` 模式默认使用：`ZCOROUTINE_PERF_SCALE_PCT=25`。

对应本轮规模：

- scheduler_tasks=30000
- channel_messages=20000
- timer_tasks=15000
- hook_rounds=10000

### 5.2 cachegrind 关键指标

- I refs: 1,339,986,187
- I1 misses: 3,801,431（0.28%）
- D refs: 871,507,278
- D1 misses: 4,349,351（0.5%）
- LL misses: 1,588,370（0.1%）
- Branches: 71,002,349
- Mispredicts: 717,127（1.0%）

结论：数据缓存 miss 总体可控（D1 miss rate 0.5%），但分支误预测约 1.0%，且调用热点显示锁/内存分配路径成本较高。

### 5.3 callgrind 热点摘要

前列热点包括：

1. `pthread_mutex_lock` / `pthread_mutex_unlock`
2. `_int_malloc` / `_int_free`
3. `std::function<void()>::function(std::function<void()>&&)`
4. `std::shared_ptr` 相关复制/析构路径

结论：锁竞争与对象生命周期（`std::function`、`shared_ptr`）仍是后续优化重点。

## 6. 权限与执行策略（perf_event_paranoid）

脚本已支持两种方式：

1. 临时调低内核限制（推荐）
2. 使用 `sudo perf`

示例：

```bash
# 方式1：优先尝试临时 sysctl（脚本会在退出时恢复）
PREFER_SYSCTL=1 ./zcoroutine/tests/stress/run_stack_model_perf_profile.sh perf

# 方式2：不修改内核参数，直接 sudo perf
PREFER_SYSCTL=0 USE_SUDO_PERF=1 ./zcoroutine/tests/stress/run_stack_model_perf_profile.sh perf
```

手工方式：

```bash
sudo sysctl -w kernel.perf_event_paranoid=-1
# ...运行 perf...
sudo sysctl -w kernel.perf_event_paranoid=4
```

## 7. 风险与后续建议

1. `hook_socketpair` 吞吐波动，建议做多轮（>=5 次）稳定性对比后再确认收益。
2. 进一步优化候选：降低 `register/unregister fiber` 哈希表路径开销、减少 `shared_ptr` 高频复制、降低 `std::function` move 构造成本。
3. 若迁移到支持 PMU 的原生 Linux 环境，建议补采 `cycles/instructions/cache-misses` 形成更完整 CPU 指标闭环。
