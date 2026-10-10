zmalloc 4 KiB 缓存策略优化与验证 · 2026-10-10

合并说明：本报告中的 worktree 路径、未提交状态及源码哈希是测量时记录。
优化按功能分批提交并合入 `main`；共享库 ABI 升级为 2，消费目标必须重新编译。
合并版本新增 `ZLYNX_ZMALLOC_AUTO_RELEASE` 构建开关，默认 OFF；自动回收性能数据
对应启用策略，不代表默认关闭时的行为。原始测量资料继续保留在持久归档目录。

后续四项已完成，见 [后续优化与验证](optimization-followup-20261010.md)。本报告保留第一轮实现范围与原始测量结论。

本次根据完整分析报告的第一优先项，优化同一 4 KiB 大小类的中心缓存往返。基线为 `2c6f5f6c2d94a4b8fc0de50dc4e4f73eeaf6da07`，修改位于分支 `perf/zmalloc-20261010` 的独立 worktree `/home/betty/repositories/zlynx-zmalloc-opt`。本次修改均位于该 worktree，未写入主工作区。

成功条件是：4 KiB 同规格批量负载出现超过测量波动的吞吐改善，跨线程释放不出现已确认的稳定退化，缓存容量有界，并通过批量、预算、Span 生命周期、并发及内存回收检查。全部候选逐步测试，基线和候选均独立构建；最终保留以下策略。

- 对齐后的 4 KiB 类（请求 3969–4096 B）建议批量由 16 增至 64。现有“四批上限”相应从 64 个对象增至 256 个对象，最大 1 MiB，仍沿用现有每线程 1 MiB 软预算；新 Span 从 8 个分配器页增至 32 页，以容纳整批。
- TransferCache 每类容量至少容纳一批，仍受 2048 个指针槽位的物理上限约束。当前仅 4 KiB 类的有效容量改变，由 16 个对象 / 64 KiB 增至 64 个对象 / 256 KiB；测试遍历所有大小类确认其他容量保持基线值。
- 补充大小类边界和查表一致性检查，覆盖 64/256/512 个对象的重复批量分配、唯一性、内容、缓存预算及保留存活 Span。原有传输缓存测试仍覆盖不足一批的命中和满缓存回退。

第一轮只把批量从 16 增至 32，4 KiB 主场景中位吞吐为基线的 1.10–1.38 倍。第二轮只增至 64，完整矩阵中批量吞吐明显改善，但跨线程释放由 33.61 降至 27.50 Mpairs/s（-18.2%）。随后单独增加传输容量至一整批，消除这一已测退化。上述中间方案未保留，原始数据保存供核查。

测量使用分析报告的原始 `probe.cc`，Clang 21.1.8、C++17、Release `-O3 -DNDEBUG -g -fno-omit-frame-pointer`，共享库和原有 initial-exec TLS，关闭 coverage。输入、线程绑核、每个 worker 预热 200 批及计时边界与原报告一致。每组前后版本交替运行，场景顺序固定种子打乱；最终矩阵为 34 个场景 × 5 次 × 2 个版本，共 340 次。所有正式探针的绑核错误数为零。

表中的吞吐为五次中位数，单位为百万次“分配+释放”/秒。4 KiB 主场景统一 batch=256、rounds=10000；时间 CV 和峰值 RSS 为前 / 后版本。这里是本轮交替对照，不将原报告不同时段的基线直接作为前后对比。

| 线程 | 优化前 Mpairs/s | 优化后 Mpairs/s | 倍率 | 时间 CV 前 / 后 | 峰值 RSS MiB 前 / 后 |
| --- | --- | --- | --- | --- | --- |
| 1 | 32.69 | 71.50 | 2.19x | 7.8% / 16.2% | 6.61 / 6.62 |
| 2 | 33.23 | 90.79 | 2.73x | 7.7% / 8.8% | 7.66 / 7.91 |
| 4 | 27.84 | 120.95 | 4.34x | 5.2% / 5.5% | 9.79 / 10.23 |
| 8 | 23.20 | 131.31 | 5.66x | 5.9% / 8.6% | 13.07 / 14.77 |
| 16 | 18.41 | 104.88 | 5.70x | 6.5% / 16.3% | 19.07 / 23.12 |

收益在各线程数中均超过本组测量波动，但仍存在中心竞争。额外一组同参数 `perf cpu-clock:u` 采样中，两个中心搬运函数的自身样本占比合计由 81.28% 降至 42.44%，进程 user CPU 从 6.84 s 降至 0.90 s。前后分别 6865 / 900 个样本、均无丢样；包含启动和预热，属于辅助证据，不能把比例直接当作锁等待时间或性能矩阵结果。

| 补充负载 | 优化前 Mpairs/s | 优化后 Mpairs/s | 倍率 | 时间 CV 前 / 后 |
| --- | --- | --- | --- | --- |
| mixed / 1 线程 | 47.90 | 48.80 | 1.02x | 6.9% / 16.4% |
| mixed / 8 线程 | 73.09 | 72.57 | 0.99x | 8.3% / 25.0% |
| 4 KiB 跨线程释放 | 33.64 | 54.34 | 1.62x | 4.2% / 38.5% |
| 4 KiB 短命线程 | 3.90 | 4.16 | 1.07x | 29.0% / 23.9% |
| 4 KiB override / 1 线程 | 28.93 | 62.11 | 2.15x | 3.4% / 5.7% |
| 4 KiB override / 8 线程 | 23.74 | 124.01 | 5.22x | 5.3% / 11.3% |

其余矩阵包含 64 B 的 1/2/4/8/16 线程、即分即释、相邻 3968/4224 B、4 KiB 的 batch 64/128/512，以及 64 B 的 override、single/cross/churn。完整中位数、范围、CV 和 RSS 在 `comparison-summary.csv` 及 JSON 中。

短时矩阵的 64 B / 2 线程中位数低 11.3%，4 KiB / batch 64 / 单线程低 6.1%。针对这两项加长测量、各交替运行十次：

- `64-t2-long`：212.84 → 208.92 Mpairs/s（-1.8%），时间 CV 6.2% / 5.7%。
- `4096-b64-t1-long`：109.43 → 108.31 Mpairs/s（-1.0%），时间 CV 9.4% / 17.2%。

加长复测未确认稳定退化，不能据此排除小幅回归。其他中位数的小幅变化也不作为收益宣传。churn 包含线程创建和退出、cross 包含槽同步与 yield；这两项不是纯分配器微基准。尾延迟记录仍为整个 batch 的抽样分位数。

内存代价有明确上限。在 32768 个 4 KiB 对象全部写满的五次实验中，批量释放后当前线程缓存由 64 KiB 增至 256 KiB，TransferCache 由 64 KiB 增至 256 KiB；合计多保留 384 KiB。主矩阵 16 线程峰值 RSS 中位数约多 4.05 MiB。其他大小类会继续共享现有线程软预算，因此这是吞吐与驻留/碎片之间的权衡。

五次前后试验都回收了完整 128 MiB，两个缓存归零，受管虚拟映射保留 128 MiB，复用内容检查全部通过。`release_memory` 后 RSS 中位数为 5.97 / 5.87 MiB，回收时间中位数 4.681 / 4.742 ms；没有观察到回收能力退化。`release_memory` 仍只清理调用线程的 TLS，其他线程须自行清理；这一行为未改变。

验证结果：

| 构建 | CTest 通过 | 范围 |
| --- | --- | --- |
| 新构建基线 Release | 17/17 | 基线全部目标 |
| 最终 Release | 17/17 | 全部 zmalloc 目标，包含普通/共享库 override、并发和冷启动 |
| ASan + UBSan | 14/14 | Debug + -O1，正常分配器路径；排除 override |
| TSan | 7/7 | TransferCache、PageMap、CentralCache、ThreadCache、memory、并发和冷启动 |

没有 sanitizer 诊断，`git diff --check` 通过。ASan 不会自动为 mmap 子对象建立 redzone，本次没有新增自定义 poison；override 的 sanitizer 路径未验证，但普通与共享库 override 功能测试及独立性能进程均已运行。

本次只实施 4 KiB 批量和缓存策略，未改变公开签名、对象布局、锁及内存序。超过 1 MiB 的缓存/驱逐、TLS 快路径与 override 重复查询属于独立方案，尚未实施。没有业务流量回放、长期碎片测量或原生 Linux/部署环境验证；结果来自非独占的 WSL2 合成负载，应在目标业务环境复核。

原始数据、脚本、基线源码归档和最终源码补丁已保存到 Linux 文件系统的持久目录：

`/home/betty/repositories/zmalloc-optimization-20261010-artifacts`

其中 `batch64-transfer64-comparison.jsonl` 是最终 340 次原始记录，`followup.jsonl` 是四十次加长复测，`batch32-quick.jsonl`、`batch64-comparison.jsonl` 和 `batch64-transfer64-focus.jsonl` 保存参数探索；另有 perf 原始数据、测试日志/XML、哈希、CSV，以及用户提供的原报告与原始归档副本。`optimized.patch` 只含本次源码和测试修改，`base-source.tar.gz` 固定基线；构建目录和二进制仍位于 `/tmp/zmalloc-optimization-20261010`。

复现脚本会从固定基线和补丁生成两个独立源码目录，避免在已修改 worktree 上重编译而污染基线。需要现有 Clang、CMake、Ninja、GTest/GMock，并保留探针所需的 guest CPU 0–15。输出路径必须尚不存在：

```bash
python3 /home/betty/repositories/zmalloc-optimization-20261010-artifacts/reproduce.py /tmp/zmalloc-reproduce-20261010 --sanitizers
```

当前构建可直接查看验证结果：

```bash
ctest --test-dir /tmp/zmalloc-optimization-20261010/batch64-transfer64 --output-on-failure
/tmp/zmalloc-optimization-20261010/batch64-transfer64/probe z 4096 8 256 10000
/tmp/zmalloc-optimization-20261010/batch64-transfer64/probe memory
```
