# zco 重构实施与验证记录

日期：2026-10-08。重构依据为 [architecture-redesign.md](architecture-redesign.md)。
实施前工作树的 HEAD 为 `443322cc8e1485557578e9e10713d2fdb24df649`；
这也是本次性能对比的旧版本，不与提案中的历史源码参照提交混用。

## 实施结果

公共入口改为 `Runtime`、`Executor`、`TaskHandle`、`Deadline` 和 `Result`。
Runtime 的构造先组装所有 Worker、Reactor 和栈资源，再通过启动屏障发布工作线程。
启动失败回滚已经创建的资源和线程。停止与提交在同一个提交端点锁下线性化；
停止后返回取消结果，端点失效不会隐式重启运行时。join 在工作线程退出后释放
Worker、Reactor 和栈，TaskHandle 只保留完成状态。

私有代码按 runtime、execution、wait、io、sync 组织。Worker 独占任务记录、
执行上下文、调度状态及就绪队列；仅未创建上下文的普通任务允许窃取。
定向提交不能窃取，已经创建的上下文不迁移。等待以 TaskId 和 generation 标识，
WakeToken 弱引用本次等待，一个终态胜出；外部完成端点只传递 WaitId。

TimerQueue 接收显式时间与绝对 Deadline。同步对象在自己的保护锁内维护唯一
谓词并登记等待，普通线程与协程共享完成协议。Channel 支持 move-only 值和
逐次结果，Mutex 只有实际授予锁才发布 ready。

Descriptor 唯一拥有 fd，区分资源身份与注册身份。关闭先撤销注册、完成等待，
再关闭 fd；替换产生新身份。Reactor 只处理资源注册与就绪事件，epoll 注册失败
回滚用户态状态。I/O 注册由作用域对象负责撤销，异常路径也清理注册。
read/write 的单次和聚合操作保留进度、EOF、错误，重试使用同一个绝对预算。
socket 默认超时在操作开始查询，没有长期元数据缓存。

Continuation 在调度栈上保存共享栈快照，切换时保存并恢复 errno。
快照分配失败通过仍驻留的用户栈抛出，使局部析构正常运行。独立栈缓存仅保留
每个 Worker 最多一块已结束任务的缓冲，没有任务、上下文或身份复用。
提交基准发现反复映射的开销后，采用最小的一块缓冲缓存并测量整体吞吐，
没有比较更大的缓存容量；容量约束另有测试。
提交队列及完成通知仅在空转为非空时唤醒，空状态判断与插入共用队列锁。

znet 显式注入 Runtime/Executor，Socket 组合 Descriptor，网络创建、连接、
接受和数据报策略留在 znet。ConnectionActor 删除全局回退；队列任务取消及
处理器异常均传回调用方。Acceptor 的挂起操作保留监听 Socket 生命周期。
TcpConnection 关闭先通过 shutdown 唤醒正在进行的读写，再在 Actor 中完成资源关闭。
TLS 等待使用类型化结果，握手、读、写和关闭各自复用操作开始时的 Deadline。
zhttp 将配置映射到实例 RuntimeOptions，不修改进程全局配置。

旧 sched、hook、Fiber、Processor、RuntimeManager、对象身份池及旧 internal
目录已经删除，仓库生产代码没有旧 API 或兼容入口。旧类型测试重写为能力测试。
核心只依赖 Threads；znet/zhttp 显式声明 zlog 依赖。zco、znet、zhttp 和根项目的
版本改为 2.0.0，相关共享库的 ABI 主版本为 2。

## 实际验证

以下计数是 CTest 可执行目标数量，不是单个 GTest 用例数量。zco 重写后的目标按
能力合并为 7 个 unit 与 3 个 integration，不能与旧目标数直接比较覆盖程度。

| 构建/检查 | 配置与结果 |
|---|---|
| 全项目 Debug | Linux x86_64，Clang 21.1.8，C++14，shared，allocator override ON；74/74 通过 |
| 全新全项目 Release | shared，allocator override OFF；74/74 通过 |
| 独立 zco Debug | static，不需要 zlog；10/10 通过 |
| 独立 zco Release | Clang 21.1.8 / GCC 15.2，C++14、extensions OFF、static、测试/perf OFF；构建通过 |
| 独立 perf 构建 | C++14，测试 OFF，perf ON，禁用 GTest 查找；zco_performance 构建通过 |
| 独立安装消费 | find_package(zco 2)，只链接 zco::zco；C++14 编译并运行成功，未引入 zlog/fmt |
| 全项目安装消费 | find_package(zhttp 2)，只链接 zhttp::zhttp；显式构造 TcpServer/HttpServer，编译并运行成功 |
| 安装头文件 | 12 个头文件逐个以 C++14 独立包含通过；没有旧 internal 或私有 src 头文件 |
| 基准及脚本 | 两种栈模型的真实定时、提交、yield、Channel、I/O 完成；脚本语法及 baseline 模式通过 |
| 源码检查 | 生产源码无旧 zco 接口/include、TODO 或临时 adapter；本次修改通过空白检查（排除工作树已有 .clang-format 修改） |

Debug 的 zco 测试按项目惯例链接 `zco_stdalloc`；allocator override ON 的生产库
同时参与 znet/zhttp 集成。Release 和独立消费验证使用系统 allocator。
Debug 使用已有构建目录；Release 与独立 zco 最初在新的构建目录配置。

核心回归覆盖与证据位置：

| 能力 | 仓库内测试 |
|---|---|
| 提前完成、切换中完成、最多一次入队、旧等待代次 | wait_state_test、task_scheduling_test；切换窗口通过 swapcontext 包装控制，重复 300 次 |
| signal/timeout/close/stop 四方竞争、线程等待 | wait_state_test |
| 明确启动失败与回滚、提交停止竞争、重复停止、自 join、完成后捕获释放 | runtime_lifecycle_test；pthread_create/epoll_create1 故障注入和 fd 数检查 |
| 无限 Event/Mutex/WaitGroup/Channel/I/O/Timer 等待取消，未执行任务取消 | runtime_shutdown_test |
| 普通任务动态窃取、绑定任务与上下文保持 Worker、yield 公平性 | task_scheduling_test；FakeReactor 控制 Worker 状态 |
| 两种栈、单个共享槽、嵌套调用、errno、无效配置、缓存容量 | stack_arena_test |
| getcontext 与快照分配失败，失败后仍可运行，局部析构执行 | task_scheduling_test |
| Event 重置语义、混合线程 Mutex/Channel、锁授予、计数、关闭和逐次结果 | synchronization_test |
| 显式时间、同截止时间、取消、大跳变、无限等待不登记 timer | timer_queue_test |
| 组合注册冲突、内核注册失败回滚、FakeReactor 旧事件、线程等待拒绝 | io_operations_test |
| fd 重用、move/duplicate、替换取消旧等待、旧注册不能唤醒新等待 | descriptor_lifecycle_test |
| 部分读写、EOF、超时进度、内核 socket 默认超时变化 | socket_io_test |
| 零长度数据报与来源地址 | znet socket_test |
| Actor 队列取消、重入与异常；关闭已挂起的无限读；TLS 总等待预算 | znet connection_actor_test、tcp_connection_test |
| TCP/TLS/HTTP/WebSocket 调用链 | 已迁移的 znet/zhttp 集成测试 |

复现构建命令示例，在仓库根目录执行：

```bash
cmake --build build/debug -j 4
ctest --test-dir build/debug --output-on-failure -j 4

cmake -S . -B /tmp/zlynx-redesign-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DBUILD_TESTING=ON \
  -DZLYNX_USE_ZMALLOC_OVERRIDE=OFF -DZLYNX_BUILD_PERF_TESTS=ON
cmake --build /tmp/zlynx-redesign-release -j 4
ctest --test-dir /tmp/zlynx-redesign-release --output-on-failure -j 4

cmake -S zco -B /tmp/zco-redesign-standalone -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=ON
cmake --build /tmp/zco-redesign-standalone -j 4
ctest --test-dir /tmp/zco-redesign-standalone --output-on-failure -j 4
cmake --install /tmp/zco-redesign-standalone --prefix /tmp/zco-redesign-install
```

安装消费工程须设置 CMAKE_PREFIX_PATH 为对应安装目录，并显式使用
`CMAKE_CXX_STANDARD=14`、`CMAKE_CXX_STANDARD_REQUIRED=ON`、
`CMAKE_CXX_EXTENSIONS=OFF`。完整 shared 安装的运行测试设置 LD_LIBRARY_PATH
指向安装目录的 lib。

## 性能结果

测量配置、配对样本及结果见下方；这次重构不承诺吞吐提升。
性能差异仍然是交付限制，功能测试通过不替代生产负载验证。

同机：Intel Core i5-12500H，16 个逻辑 CPU，Linux x86_64，Clang 21.1.8。
两边均为 Release、`-O3 -DNDEBUG -std=c++14`、static、系统 allocator，2 个 Worker，
256 KiB 栈与 8 个共享槽。旧版使用 zco_stdalloc，关闭日志；新版使用不含统计计数的
生产 zco。旧源码通过 git archive 提取上述 HEAD，未修改旧调度实现。

配对测量使用同一份短程序，按旧/新顺序交替运行三次：提交 10,000 个任务并通过
WaitGroup 等待完成；一个协程 yield 10,000 次；容量 64 的 Channel 在两个协程间
传递 10,000 项；一个协程登记并完成 100 次 1 ms 等待；非阻塞 socketpair 在两个
协程间传递 10,000 个 64 字节块。两种栈模型分别执行同样的负载。旧接口仅在临时
测量程序中适配，仓库没有保留运行时兼容层。

以下为三次吞吐中位数，单位 operations/s。

| 栈模型 | 场景 | 旧版 | 新版 | 变化 |
|---|---|---:|---:|---:|
| shared | submit | 2,514,050 | 753,519 | -70.0% |
| shared | yield | 1,098,210 | 2,420,070 | +120.4% |
| shared | channel | 1,582,550 | 11,706,800 | +639.7% |
| shared | timer | 857 | 862 | +0.6% |
| shared | io | 1,179,530 | 1,013,270 | -14.1% |
| independent | submit | 2,183,340 | 834,201 | -61.8% |
| independent | yield | 1,103,000 | 2,459,210 | +123.0% |
| independent | channel | 1,691,360 | 11,378,700 | +572.8% |
| independent | timer | 845 | 873 | +3.3% |
| independent | io | 1,119,000 | 1,447,350 | +29.3% |

提交在两种栈模型下明显下降，共享栈 I/O 的中位数也下降。yield 的样本吞吐提高。
Channel 和 I/O 样本波动较大；任务放置可能影响结果，这是推断，没有固定 CPU 或
隔离该因素，不能将中位数解释为通用的吞吐保证。新实现没有复用任务身份，并新增
可观察完成状态及等待协议；尚未逐项量化这些机制的开销。

新基准还单独记录 C++ new 分配次数、共享栈保存与恢复的复制字节、timer 到期后的
延迟分位数以及 join 前后 RSS。仅基准专用静态库启用复制计数，生产库没有这些统计。
这些统计包含基准自身的容器和完成句柄开销；new 次数不包含所有 malloc/mmap。
RSS 包含用户态 allocator 保留内存，join 后 RSS 不一定降低；关闭释放验证另由
资源生命周期测试完成。旧版没有同口径的分配、复制和尾延迟统计，不能据此比较提升。

基准专用 C++14 Release 构建的单次输出，`ZCO_PERF_SCALE_PCT=10`。
其中 submit 保留所有 TaskHandle 并逐个 join，Channel 为协程生产、普通线程消费，
与上面的配对负载不同；计数也会增加开销，所以不与配对吞吐混算：

```text
stack=shared
submit operations=10000 seconds=0.0917059 ops/s=109044 new_allocations=61039 snapshot_bytes=0
yield operations=10000 seconds=0.00425483 ops/s=2.35027e+06 new_allocations=164 snapshot_bytes=3840000
channel operations=10000 seconds=0.00563267 ops/s=1.77536e+06 new_allocations=659 snapshot_bytes=121440
timer operations=100 seconds=0.123067 ops/s=812.563 new_allocations=411 snapshot_bytes=67200
timer_latency operations=100 seconds=0.125807 ops/s=794.867 new_allocations=409 snapshot_bytes=70400
timer_lateness_us p50=241.427 p95=428.916 p99=497.516 max=594.223
io operations=10000 seconds=0.0157189 ops/s=636178 new_allocations=588 snapshot_bytes=111328
rss_kib before_stop=12192 after_join=12256
stack=independent
submit operations=10000 seconds=0.00644415 ops/s=1.55179e+06 new_allocations=61024 snapshot_bytes=0
yield operations=10000 seconds=0.0039773 ops/s=2.51427e+06 new_allocations=163 snapshot_bytes=0
channel operations=10000 seconds=0.00739991 ops/s=1.35137e+06 new_allocations=679 snapshot_bytes=0
timer operations=100 seconds=0.119065 ops/s=839.875 new_allocations=411 snapshot_bytes=0
timer_latency operations=100 seconds=0.126153 ops/s=792.689 new_allocations=409 snapshot_bytes=0
timer_lateness_us p50=266.44 p95=422.926 p99=491.395 max=506.885
io operations=10000 seconds=0.0093586 ops/s=1.06854e+06 new_allocations=90 snapshot_bytes=0
rss_kib before_stop=12512 after_join=12504
max_rss_kib=12284
```

## 剩余限制

- 实际执行环境为 Linux x86_64/Clang 21.1.8。Linux aarch64 有上下文实现，未在
  本次运行验证；其他平台在构建时拒绝，未实现跨平台运行时。
- 未执行 ASAN/TSAN、Valgrind、perf 采样和长时间生产负载。ucontext 与共享栈对
  动态检测工具有支持限制，当前通过结果不构成这些检测的结论。
- 取消需要用户代码合作。无限不让出或持续忽略取消的任务会阻止 join；从所属
  Worker 销毁 Runtime 不满足析构契约，应由外部所有者停止并 join。
- 原生 fd 只能借用，不能直接 close 或通过 dup2/dup3 覆盖。资源替换须使用
  Descriptor 操作；同步对象及借用缓冲必须活到操作结束。
- 配对吞吐仅为短基准，未固定 CPU，任务放置及系统调度有变化。性能下降已经
  记录，尚未解释所有开销来源；没有引入身份池或复杂缓存来掩盖差异。
