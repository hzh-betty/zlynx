# zco 完成状态分配优化与验证

这是首轮优化的历史记录。等待队列、调度与 fd 契约的后续实现及 ABI 升级，见
[后续优化验证记录](performance-followup-20261010.md)；下文测量与版本条件仅对应首轮。

日期：2026-10-10。基线提交为 `2c6f5f6c2d94a4b8fc0de50dc4e4f73eeaf6da07`。
独立 worktree 为 `/home/betty/repositories/zlynx-zco-opt`，分支为
`perf/zco-20261010`。

依据是 `/tmp/zco-analysis-HAtUaVuA/report.md` 及同目录的
`zco-analysis-artifacts.tar.gz`；已核对归档中的报告与原报告一致。
本轮按照报告建议，一次落实一个运行时优化：私有 Completion 的等待队列延迟构造。
成功条件是无挂起 join 的任务省去两次空队列分配，同时保留完成发布、异常、取消、
超时重试、并发 join 和安装消费行为。

## 实现与适用范围

Completion 的 `WaitQueue` 改为 C++17 `std::optional<WaitQueue>`。
仅在 `TaskHandle::join()` 发现任务尚未完成时，在原 completion mutex 内构造队列、
登记 ticket；`finish()` 在同一把锁内发布结果并唤醒存在的等待队列。
队列不在 `finish()` 中销毁，正在恢复的 joiner 仍可安全移除自己的 ticket。
构造失败继续抛出 `std::bad_alloc`，不改变任务状态；后续 join 可以重试。

无需挂起 join 的任务不再构造空 deque。首次需要等待的 join 仍会分配其队列，
后续等待复用同一个队列。本机 libstdc++ 中 Completion 大小由 144 B 增至 152 B，
WaitQueue 仍为 80 B，WaitState 仍为 128 B。
如果每个任务都需要挂起 join，队列的两次分配仅被延后，不能承诺分配次数收益；
这类完成对象还增加 8 B。

公共头文件、类布局、接口及 ABI 主版本保持原样。没有新增依赖或调整编译器优化默认值。
报告中等待者取消的 O(N²)、提交选择/广播/窃取、fcntl 查询和 ucontext 成本仍存在。
等待队列改造涉及安装头文件的布局；fd 缓存涉及外部修改非阻塞状态的语义；
替换上下文涉及信号、ABI 和检测工具。它们没有混入本次分配优化。

## 验证与基准修正

新增 `task_completion_test`，覆盖无等待者的分配计数、首次 join 的三处分配失败、
完成与首次登记/多线程 join 竞争、两种栈的多个协程 join 同一失败任务，以及超时后重试。
分配失败测试分别注入 deque map、首块、WaitState 构造失败；验证失败后任务仍可完成。

`runtime_lifecycle_test` 保留 pthread_create 故障注入：存在 Sanitizer 的弱拦截入口时，
正常创建线程经该入口登记，否则仍用 `dlsym(RTLD_NEXT)`。
这修复了原报告中 ASan/TSan 的线程登记失败，没有过滤或删除启动回滚测试，
生产运行时没有添加检测工具依赖。

现有性能基准增加同 Runtime 的 2,000 次提交预热、句柄 vector reserve、完成状态检查、
Channel 数量/顺序检查，以及 I/O 完整字节数、EOF 和内容检查。
`ZCO_PERF_STACK_MODEL=shared|independent` 可让每个进程仅测一种栈；未设置时仍测试两种。
这些修正会改变历史计时口径，本轮对比两侧使用同一份修正后的基准源码。

| 检查 | 实际结果 |
|---|---|
| Clang Release，C++17、系统 allocator | 11/11 CTest 目标、47 个 GTest 用例通过 |
| Release `--repeat until-fail:20` | 全部通过，220 次目标运行 |
| 独立 UBSan / ASan / TSan，O1、halt-on-error | 各 11/11 通过，包含启动失败回滚测试 |
| 独立共享库构建与安装 | 原版和优化版均成功 |
| GCC 外部 `find_package(zco 2)` 消费 | 两种栈、Channel 顺序与数量、join 后再 join 通过 |
| 原头文件/原共享库链接的旧消费者二进制 | 不重编译，分别加载原库和新库均通过 |
| 安装头文件树与公共 zco 符号集合 | 两侧一致 |
| `git diff --check` | 通过 |

ASan 仍警告不完全支持 makecontext/swapcontext，并忽略部分 no-return 栈请求。
本次没有接入 Sanitizer fiber API；套件通过不代表共享栈内存访问已获完整检测覆盖。
未验证 aarch64、完整 znet/zhttp 业务链、部署 allocator override 或持续业务压力。

## 分配实测

专用生产库探针用一个被外部 gate 暂停的 Worker 保持 10,000 个 pinned 任务尚未执行，
句柄 vector 预先 reserve。只统计提交阶段的 C++ new 次数和请求字节，不包含任务执行。
两种栈各重复三次，所有结果一致；计时与分配统计分开。

| 每种栈，10,000 次提交 | 原版 | 优化版 | 差值 |
|---|---:|---:|---:|
| new 次数 | 31,008 | 11,008 | −20,000，恰好每任务 −2 |
| new 请求字节总量 | 7,880,672 B | 2,200,672 B | −5,680,000 B，每任务 −568 B |
| 首次未完成 join 的新增分配 | 1 | 3 | 延迟构造队列增加 2 次 |
| 超时后再次 join 的新增分配 | 1 | 1 | 队列复用 |

每任务省下的是 64 B deque map 和 512 B 首块，抵消 Completion 增加的 8 B，
合计减少 568 B 请求字节。这里只统计 C++ new，不能当作所有 malloc/mmap 的总量。

完整基准按单独进程/单种栈运行，每侧五次、交替先后顺序，submit 为 100,000 个任务：

| 栈 | 原版 submit new 中位数 | 优化版中位数 | 减少 |
|---|---:|---:|---:|
| shared | 609,777 | 410,028 | 32.8% |
| independent | 609,870 | 410,047 | 32.8% |

总数包含任务执行记录、队列增长、join 等；任务与 join 的重叠不同会造成额外分配差异。
这个完整基准开启了跨线程统计，吞吐验收不使用它的计时。

## 无统计的吞吐观测

沿用原分析的生产库探针，同一份源码分别静态链接两侧 `libzco.a`，没有全局 new 计数
和快照字节计数。Clang 21.1.8、C++17，`-O3 -DNDEBUG -g -fno-omit-frame-pointer`，
系统 allocator，2 个 Worker、1 个提交线程，CPU affinity 为 `0,2,4,6`。
每进程先在同一 Runtime 预热 2,000 次，再测 200,000 个空任务；持有全部句柄，
计时包含提交和全部 join/status 校验。每侧七次，原版/优化版及栈模型先后顺序交替。
本轮编译、测试和 profiler 没有与正式对比同时执行。

| 栈/版本 | ops/s 中位数 | 最小–最大 | 吞吐 CV | 进程峰值 RSS 中位数 |
|---|---:|---:|---:|---:|
| shared 原版 | 118,653 | 108,182–131,207 | 6.9% | 161,420 KiB |
| shared 优化版 | 444,021 | 257,686–490,154 | 17.9% | 44,028 KiB |
| independent 原版 | 117,988 | 110,120–123,947 | 4.1% | 161,416 KiB |
| independent 优化版 | 399,707 | 387,595–522,013 | 12.4% | 44,028 KiB |

本次合成负载吞吐中位数比值分别为 3.74 和 3.39，进程峰值 RSS 中位数降低约 72.7%。
`time -v` 的用户/系统 CPU 时间中位数：shared 从 0.44/1.86 s 降至 0.20/0.50 s，
independent 从 0.39/1.95 s 降至 0.22/0.54 s。这些 CPU/RSS 数据覆盖完整进程，
包括预热、计时外清理和停止，不能解释为单次操作的 CPU 时间或资源泄漏结论。

保留了每次 stdout/stderr、time -v、/proc/stat、vmstat、CPU/内存压力及进程快照。
测量前曾观察到其他分析 probe 占用 CPU；开始正式对比时它已经退出，
但没有隔离宿主负载、采集本轮 Windows CPU 或固定宿主物理核/频率。
shared 优化版仍有 17.9% 吞吐 CV，不能把中位数倍率承诺给真实业务。
可信度最高的收益是省掉的两次分配及其请求字节；吞吐和 RSS 是此输入/环境的观测。

## 复现与原始数据

本轮原始记录位于 `/tmp/zco-opt-YGbcE9`，未覆盖原分析目录。
`comparison-results.json` 保存逐次输出和条件；`comparison-summary.json` 保存全部样本、
中位数、范围和 CV；`metadata.json` 保存输入报告/归档哈希、基线和环境。
`probe.cc` 是原分析的生产探针，`allocation_probe.cc` 是本轮分配探针。
构建配置、测试日志、兼容性结果及补丁包含在该目录的 `zco-optimization-artifacts.tar.gz`。

Release 构建，在本 worktree 根目录执行：

```bash
cmake -S zco -B /tmp/zco-opt-YGbcE9/lazy -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_CXX_FLAGS_RELEASE='-O3 -DNDEBUG -g -fno-omit-frame-pointer' \
  -DBUILD_TESTING=ON -DZLYNX_BUILD_PERF_TESTS=ON \
  -DZLYNX_ENABLE_COVERAGE=OFF -DZLYNX_USE_ZMALLOC_OVERRIDE=OFF
cmake --build /tmp/zco-opt-YGbcE9/lazy -j 4
ctest --test-dir /tmp/zco-opt-YGbcE9/lazy --output-on-failure
ZCO_PERF_STACK_MODEL=shared ZCO_PERF_SCALE_PCT=100 \
  /tmp/zco-opt-YGbcE9/lazy/tests/zco_performance
```

归档保留了两侧生产/分配探针和相同基准源码的可执行文件；在同一工具链环境中可直接运行
`python3 /tmp/zco-opt-YGbcE9/run_compare.py`。它会覆盖本轮同名比较结果，应先备份数据。
记录中的构建源码绝对路径需要按本地 worktree 调整。
