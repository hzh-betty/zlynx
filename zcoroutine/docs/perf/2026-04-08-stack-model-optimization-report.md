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

### 性能测试命令（3 轮）

```bash
./build/zcoroutine/tests/stack_model_perf \
  --gtest_filter=StackModelPerfStressTest.CompareStackModelAndLaunchStyleThroughput
```

### perf 采样命令

```bash
perf stat -d -d ./build/zcoroutine/tests/stack_model_perf \
  --gtest_filter=StackModelPerfStressTest.CompareStackModelAndLaunchStyleThroughput

perf record -F 99 -g -- ./build/zcoroutine/tests/stack_model_perf \
  --gtest_filter=StackModelPerfStressTest.CompareStackModelAndLaunchStyleThroughput
perf report --stdio --sort=dso,symbol --percent-limit 1
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

## 4. Verify（验证结果）

### 4.1 正确性回归

- `sched_unit`：通过
- `stack_model_perf`：通过
- `runtime_stress`：通过

### 4.2 3 轮吞吐均值

单位：tasks/s

| model | style | avg throughput | rounds |
|---|---|---:|---:|
| shared | std_function_capture | 129631.70 | 3 |
| independent | std_function_capture | 107541.82 | 3 |
| shared | unary_callable | 151020.86 | 3 |
| independent | unary_callable | 111406.35 | 3 |

### 4.3 相对收益

- shared vs independent（std_function_capture）：+20.54%
- shared vs independent（unary_callable）：+35.56%
- unary_callable vs std_function_capture（shared）：+16.50%
- unary_callable vs std_function_capture（independent）：+3.59%

结论：

- 在当前 workload 下，共享栈吞吐稳定高于独立栈。
- 新增 unary callable 启动路径在共享栈场景提升更明显，符合“减少重度 std::function 构造影响”的预期。

### 4.4 perf 热点证据

`perf report` 显示主要时间分布在协程线程启动路径下的调度循环，热点集中在：

- `zcoroutine::Processor::run_loop()`
- `zcoroutine::Processor::drain_new_tasks()`
- `zcoroutine::Runtime::register_fiber(...)`
- `zcoroutine::FiberHandleRegistry::register_fiber(...)`
- 相关 `unordered_map` 插入与互斥锁路径

这说明当前短任务场景下，task->fiber 实体化与句柄注册路径是关键开销来源，后续可继续围绕注册表结构和锁竞争做增量优化。

## 5. 风险与局限

- 本机 `perf stat` 对部分硬件计数器（cycles/instructions 等）显示 `<not supported>`，与当前内核/PMU权限配置有关；本报告以吞吐和 `perf record` 热点为主。
- 本轮未替换调度核心架构（如全量 lock-free、context 后端替换），因此优化是低风险增量，不是架构级重构。
- 吞吐结果受机器负载与调度噪声影响，建议在 CI 专用性能机上做长期追踪。

## 6. 后续建议

1. 将 `stack_model_perf` 扩展为可参数化（tasks/yield_interval/threads）并接入 nightly 性能回归。
2. 针对 `register_fiber` 热点，评估句柄注册表分片/批量注册策略。
3. 在具备 PMU 权限环境复测 `cycles`、`cache-miss`、`branch-miss`，完善微架构层证据链。
