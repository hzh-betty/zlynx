# zcoroutine perf + cachegrind 性能优化报告（2026-04-08）

## 1. 目标与约束

- 目标：基于同一 workload，用 perf 定位热点并做吞吐优先优化，再用 cachegrind 评估缓存/分支行为变化。
- 约束：仅改内部实现，不改对外 API。
- 指标：主指标为吞吐（tasks/s），辅以 perf 符号热点和 cachegrind miss 指标。

## 2. 测试口径与命令

### 2.1 构建与关键回归

```bash
cmake --build build -j
ctest --test-dir build -R "fiber_handle_registry_unit|runtime_manager_unit|sched_unit|runtime_stress|stack_model_perf" --output-on-failure
```

### 2.2 吞吐（5轮）

```bash
# before
for i in 1 2 3 4 5; do
  ./build/zcoroutine/tests/stack_model_perf \
    --gtest_filter=StackModelPerfStressTest.CompareStackModelAndLaunchStyleThroughput \
    > /tmp/zcoroutine_stack_baseline_round${i}.log 2>&1
done

# after
for i in 1 2 3 4 5; do
  ./build/zcoroutine/tests/stack_model_perf \
    --gtest_filter=StackModelPerfStressTest.CompareStackModelAndLaunchStyleThroughput \
    > /tmp/zcoroutine_stack_after_round${i}.log 2>&1
done
```

### 2.3 perf 与 cachegrind

```bash
# perf（环境中 perf stat 的 PMU 事件不可用，主要依赖 record/report）
perf stat -d -d ./stack_model_perf --gtest_filter=StackModelPerfStressTest.CompareStackModelAndLaunchStyleThroughput || true
perf record -F 99 -g -- ./stack_model_perf --gtest_filter=StackModelPerfStressTest.CompareStackModelAndLaunchStyleThroughput
perf report --stdio --sort=symbol --percent-limit 1

# cachegrind
valgrind --tool=cachegrind --cache-sim=yes --branch-sim=yes \
  --cachegrind-out-file=/tmp/zcoroutine_cachegrind_before.out \
  ./stack_model_perf --gtest_filter=StackModelPerfStressTest.CompareStackModelAndLaunchStyleThroughput
cg_annotate /tmp/zcoroutine_cachegrind_before.out > /tmp/zcoroutine_cachegrind_before.txt

valgrind --tool=cachegrind --cache-sim=yes --branch-sim=yes \
  --cachegrind-out-file=/tmp/zcoroutine_cachegrind_after.out \
  ./stack_model_perf --gtest_filter=StackModelPerfStressTest.CompareStackModelAndLaunchStyleThroughput
cg_annotate /tmp/zcoroutine_cachegrind_after.out > /tmp/zcoroutine_cachegrind_after.txt
```

## 3. 本次实施的优化

1. Processor 就绪队列 shared_ptr 传递路径 move 化
- `enqueue_ready`、`switch_to_fiber`、`dispatch_resumed_fiber` 改按值接收并在调用点 move。
- `dequeue_ready_fiber` 从队列 front 改为 move 出队。
- 目标：减少 shared_ptr 引用计数原子增减与拷贝赋值成本。

2. FiberHandleRegistry 容量预留
- 构造中增加 `handle_map_.reserve(8192)`。
- 目标：降低高频注册/注销下 rehash 和哈希桶抖动。

3. debug 日志编译期开关
- 默认关闭 `ZCOROUTINE_LOG_DEBUG`，仅在定义 `ZCOROUTINE_ENABLE_DEBUG_LOGS` 时启用。
- 目标：降低热路径模板日志调用与格式化开销，减少 profiling 噪声。

## 4. 吞吐结果（5轮均值）

单位：tasks/s

| key | before avg | after avg | delta |
|---|---:|---:|---:|
| independent\|std_function_capture | 215291.58 | 207747.11 | -3.50% |
| independent\|unary_callable | 209950.45 | 222662.88 | +6.05% |
| shared\|std_function_capture | 305569.93 | 308794.96 | +1.06% |
| shared\|unary_callable | 328937.69 | 340463.81 | +3.50% |

观察：
- 吞吐收益主要集中在 shared 场景和 unary callable 场景。
- independent + std_function_capture 出现小幅回退，说明优化收益在不同路径分布不均。

## 5. perf 热点对比（record/report）

关键符号（Children 占比）:

| symbol | before | after | delta |
|---|---:|---:|---:|
| `zcoroutine::Processor::run_ready_tasks()` | 41.13% | 41.54% | +0.41pp |
| `zcoroutine::Processor::dispatch_resumed_fiber(...)` | 15.25% | 19.85% | +4.60pp |
| `zcoroutine::Processor::switch_to_fiber(...)` | 13.83% | 9.93% | -3.90pp |
| `zcoroutine::Processor::dequeue_ready_fiber(...)` | 7.09% | 1.47% | -5.62pp |
| `std::shared_ptr<zcoroutine::Fiber>::operator=(...)` | 7.80% | 1.10% | -6.70pp |
| `std::shared_ptr<zcoroutine::Fiber>::~shared_ptr()` | 2.84% | 7.72% | +4.88pp |

解释：
- move 化后，`dequeue_ready_fiber` 和 shared_ptr 赋值热点明显下降，符合预期。
- shared_ptr 析构占比上升，属于对象生命周期成本在火焰图中的再分配，不等于总成本上升到同等幅度（总 samples 规模已变化）。

## 6. cachegrind 前后对比

### 6.1 PROGRAM TOTALS

| metric | before | after | delta |
|---|---:|---:|---:|
| Ir | 6788331922 | 5938155258 | -12.52% |
| I1mr | 32023023 | 7391849 | -76.92% |
| ILmr | 27107 | 26043 | -3.93% |
| Dr | 2356833915 | 2050483161 | -13.00% |
| D1mr | 8075648 | 8227772 | +1.88% |
| DLmr | 2256496 | 2176307 | -3.55% |
| Dw | 2015540644 | 1750922197 | -13.13% |
| D1mw | 15591675 | 15214753 | -2.42% |
| DLmw | 10849717 | 10979340 | +1.19% |
| Bc | 363099776 | 322171687 | -11.27% |
| Bcm | 5465054 | 4191533 | -23.30% |
| Bi | 30202697 | 29265798 | -3.10% |
| Bim | 269653 | 1404250 | +420.77% |

### 6.2 关键比率

| rate | before | after | delta |
|---|---:|---:|---:|
| I1 miss rate = I1mr/Ir | 0.4717% | 0.1245% | -0.3472pp |
| D1 read miss rate = D1mr/Dr | 0.3426% | 0.4013% | +0.0587pp |
| D1 write miss rate = D1mw/Dw | 0.7736% | 0.8689% | +0.0953pp |
| LL data miss rate = (DLmr+DLmw)/(Dr+Dw) | 0.2998% | 0.3461% | +0.0463pp |
| Conditional branch miss = Bcm/Bc | 1.5051% | 1.3010% | -0.2041pp |
| Indirect branch miss = Bim/Bi | 0.8928% | 4.7986% | +3.9058pp |
| Overall branch miss = (Bcm+Bim)/(Bc+Bi) | 1.4581% | 1.5922% | +0.1341pp |

解释：
- 指令侧收益显著（Ir 与 I1 miss 大幅下降），与默认关闭 debug 日志高度一致。
- 数据缓存 miss 率（D1/LL）略有上升，说明热路径结构访问模式仍有改进空间。
- 条件分支预测改善，但间接分支误预测明显上升，需要后续拆分调用路径进一步分析。

## 7. 结论

1. 已完成 perf 热点定位、代码优化、cachegrind 对比与回归验证闭环。
2. 吞吐层面：整体呈结构性改善（shared 与 unary 场景收益明显），但 independent + std_function_capture 有小幅回退。
3. 热点层面：shared_ptr 赋值/出队热点显著下降，说明 move 化优化有效。
4. 缓存层面：指令缓存显著改善，数据缓存与间接分支仍是下一轮优化重点。

## 8. 下一步建议

1. 针对 `dispatch_resumed_fiber` 细分子路径做二次 perf 采样，区分 unregister、recycle、状态分发成本。
2. 针对 `FiberHandleRegistry` 继续评估分片或无锁读优化，重点压低 D1/LL data miss。
3. 单独压测 independent + std_function_capture 路径，确认回退是否来自任务分布抖动或真实路径退化。
4. 如需更稳定微架构指标，迁移到支持完整 PMU 事件的机器复测 `perf stat`。