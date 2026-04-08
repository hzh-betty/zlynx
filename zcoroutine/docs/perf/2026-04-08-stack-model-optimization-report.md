# zcoroutine 栈模型性能优化报告（2026-04-08）

## 1. Target（目标）

- 目标 1：在相同负载下对比共享栈与独立栈吞吐差异。
- 目标 2：降低任务提交路径中 std::function 的构造/类型擦除影响。
- 目标 3：给出可复现的测试命令、性能数据和热点证据。

本轮主指标：吞吐（tasks/s）。

## 2. Profile（基线采样与命令）

### 构建与测试命令

```bash
cmake -S zcoroutine -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build -R sched_unit --output-on-failure
ctest --test-dir build -R stack_model_perf --output-on-failure
```

### 性能测试命令（5 轮）

```bash
for i in 1 2 3 4 5; do
  ./build/zcoroutine/tests/stack_model_perf \
    --gtest_filter=StackModelPerfStressTest.CompareStackModelAndLaunchStyleThroughput \
    > /tmp/stack_bench_round${i}.log 2>&1
done

rg "\[stack-model-bench\]" /tmp/stack_bench_round*.log
```

### perf 采样命令

```bash
perf stat -d -d ./build/zcoroutine/tests/stack_model_perf \
  --gtest_filter=StackModelPerfStressTest.CompareStackModelAndLaunchStyleThroughput

perf record -F 99 -g -- ./build/zcoroutine/tests/stack_model_perf \
  --gtest_filter=StackModelPerfStressTest.CompareStackModelAndLaunchStyleThroughput
perf report --stdio --sort=dso,symbol --percent-limit 1

valgrind --tool=cachegrind --cache-sim=yes --branch-sim=yes \
  --cachegrind-out-file=/tmp/cachegrind.stack_model.cache.out \
  ./build/zcoroutine/tests/stack_model_perf \
  --gtest_filter=StackModelPerfStressTest.CompareStackModelAndLaunchStyleThroughput
cg_annotate /tmp/cachegrind.stack_model.cache.out
```

## 3. Optimize（实施的优化）

### 3.1 API 优化（减少 std::function 直接使用）

新增如下协程启动入口（保持原 go(Task) 兼容）：

- `go(Closure* cb)`：执行后自动释放 Closure。
- `go(F&& f)`：无参可调用对象。
- `go(F&& f, P&& p)`：单参数可调用对象（支持 `f(p)` 与 `(*f)(p)`）。
- `go(void (T::*method)(P), T* target, P&& p)`：成员函数入口。

同样为 `Scheduler::go(...)` 提供对应重载。

优化动机：把重对象捕获尽量迁移到更轻的调用方式（如函数指针+参数指针、成员函数+对象指针），减少重捕获 lambda 直接转 `std::function<void()>` 的成本。

### 3.2 新增最小性能对比程序

新增 stress 基准测试：

- 文件：`zcoroutine/tests/stress/stack_model_perf.cc`
- 对比维度：
  - 栈模型：`shared` vs `independent`
  - 启动风格：`std_function_capture` vs `unary_callable`
- 输出：`throughput_tasks_per_s`

测试固定参数：

- `scheduler_count=8`
- `producer_threads=8`
- `total_tasks=120000`
- `yield_interval=8`
- `stack_size=64KB`
- `shared_stack_num=8`

### 3.3 句柄注册路径二次优化（本次）

针对 `FiberHandleRegistry` 热点，实施以下最小改动：

- 在 `Fiber` 内新增原子 `external_handle_id` 字段。
- 删除 `FiberHandleRegistry` 的反向哈希表（`Fiber* -> handle_id`）。
- `try_get_handle_id` 改为直接读取 `Fiber` 原子字段（无锁）。
- `unregister_fiber` 在注销时原子清零句柄，再删除正向映射。
- `Runtime::register_fiber/unregister_fiber/external_handle` 去除重复查询，直接利用原子字段与注册表返回值。

优化动机：减少短任务场景下 `register/unregister/try_get_handle_id` 的锁竞争与哈希查找开销。

## 4. Verify（验证结果）

### 4.1 正确性回归

- `sched_unit`：通过
- `stack_model_perf`：通过
- `runtime_stress`：通过

### 4.2 5 轮吞吐统计

单位：tasks/s

| model | style | avg throughput | stddev | min | max | rounds |
|---|---|---:|---:|---:|---:|---:|
| shared | std_function_capture | 131381.44 | 4854.20 | 123438.34 | 137045.48 | 5 |
| independent | std_function_capture | 106176.17 | 4201.95 | 99424.45 | 111701.20 | 5 |
| shared | unary_callable | 150888.20 | 5713.59 | 141791.79 | 159586.62 | 5 |
| independent | unary_callable | 113618.57 | 4854.78 | 108308.95 | 121107.93 | 5 |

### 4.3 相对收益

- shared vs independent（std_function_capture）：+23.74%
- shared vs independent（unary_callable）：+32.80%
- unary_callable vs std_function_capture（shared）：+14.85%
- unary_callable vs std_function_capture（independent）：+7.01%

结论：

- 在当前 workload 下，共享栈吞吐稳定高于独立栈。
- 新增 unary callable 启动路径在共享栈场景提升更明显，符合“减少重度 std::function 构造影响”的预期。

### 4.4 perf 热点证据

`perf report` 显示主要时间分布在协程线程启动路径下的调度循环，热点集中在：

- `zcoroutine::Processor::run_loop()`
- `zcoroutine::Processor::run_ready_tasks()`
- `zcoroutine::Runtime::register_fiber(...)`
- `zcoroutine::Runtime::unregister_fiber(...)`
- `zcoroutine::FiberHandleRegistry::register_fiber(...)`
- `zcoroutine::FiberHandleRegistry::unregister_fiber(...)`
- `zcoroutine::FiberHandleRegistry::try_get_handle_id(...)`
- 相关 `unordered_map` 插入与互斥锁路径
- `__swapcontext` 上下文切换路径

这说明当前短任务场景下，task->fiber 实体化与句柄注册路径是关键开销来源，后续可继续围绕注册表结构和锁竞争做增量优化。

### 4.5 Valgrind（cachegrind）缓存命中证据

本轮在相同 workload 下使用 `cache-sim=yes` 采集缓存行为，总量如下：

- I refs: 8,576,679,419
- I1 misses: 85,289,029（I1 miss rate 0.99%）
- D refs: 5,578,940,209（读 2,998,992,139 + 写 2,579,948,070）
- D1 misses: 26,879,626（D1 miss rate 0.5%）
- LLd misses: 12,397,506（LLd miss rate 0.2%）
- LL misses: 12,426,061（LL miss rate 0.1%）
- Branches: 468,446,189，Mispredicts: 6,465,572（mispred rate 1.4%）

`cg_annotate` 热点摘要显示：

- `hashtable_policy.h` 约 12.1% Ir，占比最高，且 D1/LL miss 持续出现。
- `shared_ptr_base.h` 约 10.8% Ir，说明引用计数与对象生命周期管理仍是热路径成本。
- `malloc.c` 约 8.0% Ir 且分支误预测占比高，表明高频分配/释放仍有压力。
- `std_function.h` 约 5.9% Ir，与任务封装路径的类型擦除成本一致。

结合 perf 与 cachegrind，证据链一致指向：**Fiber 句柄哈希表 + shared_ptr 生命周期 + 分配器路径** 是当前短任务吞吐上限的关键约束。

### 4.6 二次优化后复测（5 轮）

单位：tasks/s

| model | style | avg throughput | stddev | min | max | rounds |
|---|---|---:|---:|---:|---:|---:|
| shared | std_function_capture | 331506.55 | 22592.26 | 291086.64 | 360025.59 | 5 |
| independent | std_function_capture | 237642.61 | 17344.64 | 209824.42 | 263238.26 | 5 |
| shared | unary_callable | 370829.30 | 17312.17 | 350807.27 | 402128.09 | 5 |
| independent | unary_callable | 242417.40 | 11123.45 | 228951.96 | 259694.52 | 5 |

相对 4.2 的上一版 5 轮均值，本次复测提升如下：

- shared vs std_function_capture：+152.32%
- independent vs std_function_capture：+123.82%
- shared vs unary_callable：+145.76%
- independent vs unary_callable：+113.36%

说明：

- 本次收益主要来自句柄注册路径锁竞争和查找开销下降。
- 已通过 `fiber_handle_registry_unit`、`runtime_manager_unit`、`sched_unit`、`runtime_stress`、`stack_model_perf` 回归验证。

## 5. 风险与局限

- 本机 `perf stat` 对部分硬件计数器（cycles/instructions 等）显示 `<not supported>`，与当前内核/PMU权限配置有关；本报告以吞吐和 `perf record` 热点为主。
- cachegrind 结果是单次运行的全流程聚合（四种场景在同一测试内顺序执行），不能直接拆分为每个场景的独立缓存 miss 率。
- 本轮未替换调度核心架构（如全量 lock-free、context 后端替换），因此优化是低风险增量，不是架构级重构。
- 吞吐结果受机器负载与调度噪声影响，建议在 CI 专用性能机上做长期追踪。

## 6. 后续建议

| 建议 | 适用场景 | 风险 | 验证指标 |
|---|---|---|---|
| 将 `stack_model_perf` 参数化（tasks/yield/threads）并接入 nightly 性能回归 | 需要持续观察趋势、控制噪声时 | 场景增多导致 CI 耗时增加 | 5 轮均值、标准差、回归阈值告警 |
| 对 `FiberHandleRegistry` 做分片或分层索引（先小范围试验） | `register/unregister/try_get_handle_id` 占热点显著时 | 一致性与并发正确性复杂度提升 | perf 中 registry 路径占比下降、吞吐提升、回归测试通过 |
| 在具备 PMU 权限机器复测 `cycles/instructions/cache-miss/branch-miss` | 需要补齐微架构证据链时 | 环境迁移带来横向对比偏差 | IPC、cache-miss rate、branch-miss rate 与吞吐共同改善 |
