# 等待队列、调度与 fd 状态优化验证

日期：2026-10-10。实现和验证使用工作分支 `perf/zco-20261010`，当时保持未暂存、未提交。
修改先同步到主分支工作区，再按功能分批提交；保留同期 zlog 提交和内容。
优化 worktree 已删除，以下路径用于描述历史构建与测量。
本轮基线是上一轮已完成的 Completion 延迟构造代码，即提交
`2c6f5f6c2d94a4b8fc0de50dc4e4f73eeaf6da07` 加上当时的未提交修改。
基线源码已独立保存在 `/tmp/zco-opt-followup-uZnE5O/baseline-source`，不覆盖首轮原始结果。

按照等待者取消、提交选择、唤醒、窃取、fd 查询的顺序逐项实施。
每个运行时阶段分别编译、通过 CTest 并保存源码、库与探针，供相邻阶段比较。
成功条件是消除取消扫描、降低调度开销和重复查询，同时保留 FIFO、唯一完成、
任务绑定、停止线性化和描述符生命周期；fd 状态契约允许收紧，ucontext 沿用原实现。

## 实现及正确性约束

WaitQueue 使用弱引用的稳定 list 节点，WaitState 保存登记归属及节点迭代器。
`remove()` 检查归属后直接 erase，单次移除 O(1)，不依赖移除顺序。
`complete_one()` 摘除节点并清除登记归属后再完成等待，已完成 ticket 的后续 remove
不访问失效迭代器；析构也清除存活登记的归属。FIFO 和 WaitState 的单次完成锁协议保留。
queue 仍不持有 WaitState 的强引用，丢弃 ticket 后旧 token 仍会过期。
每次 add 最多检查四个条目，循环清理遗弃的过期弱引用；不会用取消路径的全队列扫描清理它们。
队列禁止复制，避免复制登记归属和迭代器。

普通提交轮转第一个候选，再比较间隔半个 Worker 集合的第二个候选，最多读取两个 Worker
的负载；从 O(W) 全量选择变为 O(1)。Executor 提交仍绑定指定 Worker。
Submission mutex 保留，继续保护 accepting、Worker 生命周期和提交/停止的线性化。

Submission 维护不分配内存的空闲 Worker 链表。目标 Worker 的队列空转非空时仍唤醒目标；
普通任务投递到未登记为空闲的目标时，额外唤醒一个已登记的空闲 Worker。
Worker 在休眠前取得 Submission mutex，并复查自身队列、停止状态及全部可窃取队列。
任务先发布时，复查阻止休眠；任务后发布时，提交方可从空闲链表取出 Worker 并唤醒，
eventfd 保留通知直到 poll 消费。poll 因 I/O、超时或唤醒返回后均解除空闲登记。
停止仍唤醒所有 Worker，join 仍等全部线程退出后销毁 Worker，避免空闲链表中的裸指针悬空。

TaskQueues 维护可窃取任务数提示。steal 在提示为零时跳过队列锁，非零时仍在锁内复查；
pinned 任务不计入该提示。Worker 轮转窃取起点，降低固定受害队列的竞争。
窃取和休眠复查仍可能扫描 O(W) 个候选，但不会对明显没有可窃取任务的队列逐一加锁。

Descriptor 接管时查询一次 F_GETFL，Resource 保存不可变的非阻塞状态，transfer 使用该状态。
接管阻塞 fd 仍允许，但协程传输拒绝它；查询失败关闭已接管 fd 并抛出错误，EINTR 重试。
资源锁仍包围 fd 检查和实际系统调用，关闭/替换同步、部分进度、EOF、EINTR 和 SIGPIPE
防护均保留。duplicate 的新资源同样在接管时查询一次。

**新的 fd 契约：管理期间不得通过 native_handle、Borrow 或任何共享文件状态的 dup 别名
修改 O_NONBLOCK。需要改变状态时，必须先结束相关操作并解除所有相关管理资源，再配置和接管。**
外部违反契约的修改不会被每次传输自动检测，可能使 Worker 执行阻塞调用。
原先接管后外部切换非阻塞状态的代码需要调整。HTTP 测试的阻塞客户端改为直接接管初始
阻塞 fd，在外部线程做原生调用，避免接管非阻塞 Socket 后清除标志。

## ABI 与分配取舍

WaitQueue 在安装头文件中按值嵌入同步类型，布局变化无法保持原二进制 ABI。
zco 从 2.0.0 升至 3.0.0；znet 的公开连接类型内嵌 Mutex，因此从 3.0.0 升至 4.0.0。
两者 SONAME 主版本相应升级，消费者及依赖它们的库需要重新编译。
znet 安装/独立构建要求 zco 3，zhttp 要求 znet 4；公共包模板支持带版本的依赖声明。

本机 x86_64/libstdc++ 布局测量：

| 类型 | 上轮基线 | 本轮 |
|---|---:|---:|
| WaitQueue | 80 B | 32 B |
| Completion | 152 B | 104 B |
| WaitState | 128 B | 144 B |
| Event / Mutex / WaitGroup | 各 128 B | 各 80 B |
| Channel<int> | 296 B | 200 B |
| TaskHandle / Descriptor | 各 16 B | 各 16 B |

每个实际入队的等待增加一个 list 节点分配，WaitState 也增加 16 B。
空队列不再需要 deque 的两次初始化分配，但不能将该改动描述为所有等待场景的分配减少。
分开开启统计的 10,000 元素 Channel 样本：shared new 次数从 659 增至 942，
independent 从 644 增至 943；这些单次样本受挂起次数影响，用于说明分配取舍，不能验收固定幅度。
单独的无统计生产库 Channel 基准五次中位数为：shared 1,937,550 → 1,936,100 ops/s，
independent 1,897,000 → 1,930,300 ops/s，未观察到超出波动的明显吞吐退化。

## 验证

新增/扩展覆盖：8,192 个存活 ticket 的乱序及重复移除、FIFO、已摘除 ticket 清理、
过期 token、队列销毁后的登记失效、信号/超时/移除竞争、两种栈各 2,000 个等待者的超时风暴；
八 Worker 阻塞负载、两个候选均阻塞时由其他 Worker 窃取、四生产者突发提交和 yield、
空闲链表中间节点移除及每次只唤醒一个已登记 Worker；
接管查询次数、重复 I/O 不再查询、duplicate、阻塞状态拒绝、查询失败关闭、非 socket 传输。
首次 join 分配失败测试更新为 WaitState 和登记节点两处分配失败。

| 验证 | 结果 |
|---|---|
| 各运行时阶段独立 Release CTest | 等待/选择/唤醒/窃取各 12/12，fd 阶段 13/13，通过 |
| 最终 zco Release | 13 个目标、61 个 GTest 用例；until-fail:20 共 260 次目标运行通过 |
| UBSan / ASan / TSan | 独立 Clang O1 构建，各 13/13 通过 |
| 整仓库 GCC Release shared，系统 allocator | 80/80 通过，含 TCP/TLS/HTTP/WebSocket |
| 同步后的主分支，包含已有 zlog 修改 | GCC Release shared 重新构建，80/80 通过 |
| 全项目安装后的外部消费 | 仅 find_package(zhttp 4) 即可解析 znet 4、zco 3；两种栈、Channel、重复 join 通过 |
| 旧版本请求 | find_package(zco 2) 被已安装的 zco 3 正确拒绝 |
| 源码/补丁 | 实现阶段 git diff --check 通过；随后按功能分批提交 |

整仓库构建有五处原有的 GCC dangling-reference 告警，位于未修改的
`zhttp/src/config/server_config.cc`；本轮没有处理这些无关代码。
主分支首次链接遇到 /tmp tmpfs 空间不足，清理本轮可重建的对象文件后重试通过，
首次失败日志也保留在归档中。
ASan 仍不完全支持 makecontext/swapcontext，没有接入 fiber API，套件通过不构成完整共享栈
内存访问证明。未验证 aarch64、默认 allocator override ON 的完整链路或长期真实业务压力。

## 性能观测

生产库 Clang 21.1.8、C++17，`-O3 -DNDEBUG -g -fno-omit-frame-pointer`，系统 allocator。
吞吐探针不启用全局 new 或快照统计，每进程仅测一种栈，先在同 Runtime 预热 2,000 次。
各配置五次，交替版本和栈顺序；正式测量期间没有本轮编译、测试或 profiler 同时运行。
保存全部逐次值、CV、范围、time -v、Linux CPU/压力/进程快照；没有隔离宿主调度、物理核或频率。

持有全部 ticket，仅计逐个 remove；固定 CPU mask 0,2,4,6：

| 等待者数 | 基线总耗时中位数 | 本轮中位数 |
|---|---:|---:|
| 1,000 | 4.748 ms | 0.0138 ms |
| 2,000 | 19.768 ms | 0.0254 ms |
| 4,000 | 81.303 ms | 0.0597 ms |
| 8,000 | 310.217 ms | 0.1275 ms |

源码的直接 erase 和近线性规模曲线共同支持复杂度改进，倍数只对应该合成输入。

相邻调度阶段，16 Worker、四生产者、independent、20,000 个空任务，CPU mask 0–15：

| 阶段 | ops/s 中位数 | CV |
|---|---:|---:|
| 等待队列优化后 | 6,561 | 4.5% |
| 增加两个候选的提交选择 | 6,742 | 5.8% |
| 增加空闲登记与定向唤醒 | 168,012 | 5.5% |
| 增加空队列窃取跳过及轮转 | 163,673 | 5.9% |

选择和窃取在这个场景的微小差异落在波动范围内，不能宣布吞吐收益或回退。
单独对比唤醒阶段与窃取阶段的 16 Worker / 500,000 次 yield，
shared 中位数 2,075,510 → 2,449,390，independent 2,097,860 → 2,493,290 ops/s，
观测提高约 18.0%/18.8%，验证跳过空队列锁在频繁让出场景的作用。

最终对比相对上轮基线，independent、20,000 个空任务（两种栈完整数据均归档）：

| Worker / 生产者 | 基线 ops/s 中位数 | 本轮中位数 | 基线 / 本轮 CV |
|---|---:|---:|---:|
| 1 / 1 | 440,877 | 476,050 | 5.3% / 4.1% |
| 1 / 4 | 842,887 | 834,282 | 3.7% / 5.6% |
| 2 / 1 | 436,224 | 1,149,800 | 22.6% / 12.9% |
| 2 / 4 | 97,498 | 526,387 | 54.2% / 21.1% |
| 8 / 1 | 13,252 | 218,592 | 3.0% / 39.7% |
| 8 / 4 | 12,055 | 212,708 | 6.2% / 18.9% |
| 16 / 1 | 6,475 | 118,155 | 3.4% / 12.1% |
| 16 / 4 | 6,724 | 183,561 | 8.6% / 15.6% |

高 Worker 场景从非常低的合成基线改善；本轮仍有明显波动，空任务比值不能外推到真实 CPU
业务。单 Worker 四生产者的约 1% 差异未超出波动，不能判定回退。

只比较 fd 查询前后（其余代码相同），socketpair、固定任务放置、字节数/EOF/内容校验：

| 模型 / Worker / 消息 | 查询前中位数 | 查询后中位数 | ops/s |
|---|---:|---:|---|
| shared / 1 / 64 B | 864,593 | 1,110,490 | 约 +28.4% |
| independent / 1 / 64 B | 873,465 | 1,138,580 | 约 +30.4% |
| shared / 2 / 64 B | 1,340,010 | 1,651,370 | 约 +23.2% |
| independent / 2 / 64 B | 1,351,080 | 1,649,630 | 约 +22.1% |

4096 B 消息也已测量，观测中位数提高约 2.9%–9.2%，部分区间重叠；完整范围/CV 见 JSON。
64 B 每配置 200,000 块，4096 B 每配置 10,000 块；这是 Unix socketpair 载荷吞吐，非 TCP/HTTP QPS。

strace 仅验证调用次数，不把插桩时间当成性能收益：

| 相邻阶段输入（含预热/停止） | 优化前调用数 | 优化后 |
|---|---:|---:|
| 20,000 个 64 B I/O 块，另预热 2,000 块：fcntl | 44,156 | 2 |
| 16 Worker / 四生产者 / 10,000 提交，另预热 2,000：write | 192,017 | 657 |

提交探针中的 write 包含 eventfd 唤醒和一次 stdout 输出；减少量支持广播消除，
不等于任何真实业务都应有固定的相同调用数。

## 原始结果与复现

记录位于 `/tmp/zco-opt-followup-uZnE5O`：`comparison-results.json` 和
`comparison-summary.json` 保存 340 次完整对比；`supplemental-results.json` 保存
40 次窃取 yield / Channel 对比，全部成功。`strace-metadata.json` 保存系统调用命令与结果。
各阶段源码、静态库、探针、构建/测试配置、日志、补丁及本文归档到
`zco-followup-artifacts.tar.gz`，源文件和归档成员有 SHA256 记录。
归档写入 `/home/betty/.cache/zco-artifacts/zco-opt-followup-uZnE5O`，原 /tmp 路径提供符号链接，
避免 /tmp tmpfs 的空间限制。

在相同 Linux 工具链环境中，保留各阶段 probe/io 二进制即可运行：

```bash
python3 /tmp/zco-opt-followup-uZnE5O/run_compare.py
python3 /tmp/zco-opt-followup-uZnE5O/run_supplemental.py
```

这些命令覆盖同名结果，复跑前先备份。构建命令保存在配置日志及 CMakeCache.txt；
当前代码构建示例：

```bash
cmake -S zco -B /tmp/zco-followup-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ \
  -DBUILD_TESTING=ON -DZLYNX_USE_ZMALLOC_OVERRIDE=OFF
cmake --build /tmp/zco-followup-release -j 4
ctest --test-dir /tmp/zco-followup-release --output-on-failure
```
