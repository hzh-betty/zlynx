# zco 架构重设计提案

- 状态：核心重构已实施；实际验证与剩余限制见 [重构验证记录](refactor-validation.md)。
- 日期：2026-10-08。
- 源码定位参照：编写文档时的仓库 HEAD `c26af4e`。
- 范围：zco 全部实现、接口、测试与构建，以及 znet/zhttp 的相关调用方。
- 约束：允许 Breaking Changes；保留核心能力；不维护两套实现或旧 API 兼容层。

本文件保留重构前的分析证据与实施设计。第 2～3 节的旧源码定位和第 11 节的分析阶段
记录来自参照提交和分析阶段；旧文件已按迁移清单删除，不应把历史记录当作新实现的
验证结果。目标 API 示例是设计示意，最终可编译接口见 README 与公开头文件。
实际实施与验收结果单独记录在 [refactor-validation.md](refactor-validation.md)。

## 1. 结论与成功条件

zco 需要重新设计运行时生命周期、等待协议和协程所有权。现有问题涉及丢失唤醒、
重复入队、关闭后的资源保留和错误传播，仅移动文件或拆分类无法解决。

重设计围绕五项能力组织：任务调度、协程执行与栈、等待与截止时间、描述符 I/O、
同步原语。Runtime 是显式资源组装入口，不再作为全局服务查找器。

完成标准：

1. 保留多线程调度、任务窃取、共享栈和独立栈、定时等待、协程 I/O、混合线程同步能力。
2. 明确所有任务和资源的创建者、拥有者、销毁者及失效条件。
3. 每次等待最多完成一次，每个协程最多拥有一份有效就绪队列项。
4. 启动和停止有明确线性化点；停止后不接受新任务，不隐式重启。
5. 核心等待、计时及同步逻辑可使用确定性输入测试，不依赖真实网络和全局 Runtime。
6. 迁移所有仓库内调用方，删除旧实现、旧接口和只为它们存在的测试结构。
7. 全项目编译与测试通过；安装后消费通过；关键并发交错有回归测试。
8. 不以增加类数量或代码量作为进展指标，新增抽象必须有明确隔离价值。

## 2. 调研范围与核心业务

已扫描全仓库的目录、构建目标和模块接口，完整阅读 zco 的公共与内部头文件、
源文件、单元及集成测试、测试支撑代码、README、CMake 和基准代码；核对 znet
连接、TCP 服务、socket、TLS 等待与 zhttp 启动配置的主要调用链。
没有声称逐行阅读所有与 zco 无关的 allocator、logger 和 HTTP 实现。

zco 是 Linux/C++14 有栈协程运行时，其领域是执行、调度、等待和资源就绪，
不是 HTTP 协议或业务服务。它提供：

- 多工作线程任务分配、负载感知和上下文创建前的任务窃取。
- 共享栈、独立栈及栈快照。
- yield、挂起、恢复、休眠与定时器。
- epoll/eventfd 驱动的协程 I/O 等待。
- 显式 `co_*` 系统调用包装；当前不是透明替换 libc 的 interposition 实现。
- 普通线程和协程均可使用的 Event、Mutex、WaitGroup、Channel。

主要构建依赖：

```text
zhttp → znet → zco → zlog → fmt / Threads
```

根构建可配置 allocator override；它属于构建及分配策略，不应成为上层业务接口。
未发现上述 CMake 目标的循环依赖。zco 内部存在对象及实现调用的循环耦合：

```text
Runtime → Processor → Runtime::instance()
Processor → Fiber → Processor
Processor → FiberStackManager → Fiber → Processor
```

这里区分构建依赖、头文件依赖和对象调用关系，不将对象双向引用误报为 CMake 循环。

### 2.1 当前模块职责

| 模块 | 实际职责 | 边界问题 |
|---|---|---|
| sched、Runtime | 全局配置、启动停止、任务分配、Scheduler 句柄、Fiber 句柄、跨线程 I/O 取消 | 生命周期、身份和调度策略集中，存在全局隐式依赖 |
| Processor | 工作线程、窃取、负载统计、就绪队列、FiberPool、定时器、I/O、上下文与栈协调 | 多个独立变化原因集中 |
| Fiber、Context | 回调、状态、上下文、栈快照、owner、外部句柄、超时标志 | 执行资源和调度协议混合，反向访问 Processor |
| 栈与对象池 | 共享栈、快照缓存、Fiber 复用 | 资源复用与对象身份复用混合，跨对象转发较多 |
| TimerQueue | 时间读取、截止时间、回调、取消 | 难以注入确定性时间，取消后的捕获释放延后 |
| Poller、Epoller | 内核事件、注册表、等待者收集 | 接口携带 Fiber 和 TimerToken |
| IoWaitService、IoEvent | 注册、超时、取消、挂起 | I/O 等待独立维护完成协议，IoEvent 缺少独立生命周期 |
| hook | 重试、超时缓存、fd 元数据、关闭取消、TCP 配置、系统调用包装 | 运行时机制与网络策略混合 |
| 同步原语 | 条件、锁、计数、队列、线程和协程等待者 | 直接参与调度，存在重复状态 |
| zco_logger | 日志初始化和各层错误记录 | 公共模板与运行时依赖具体日志实现 |

### 2.2 设计问题对应关系

| 检查项 | 具体表现 | 设计处理 |
|---|---|---|
| God Class / 单一职责 | Runtime、Processor 混合多个生命周期与策略 | 按调度、执行、等待、I/O 分离 |
| Feature Envy | Fiber 访问 Processor 的栈和快照能力 | 执行模块内部管理栈资源 |
| Shotgun Surgery | 等待修改涉及 Fiber、Timer、Poller、同步对象和 Processor | 统一等待协议与完成原因 |
| Primitive Obsession | `void*` 恢复句柄、裸 fd、整数超时哨兵、char shutdown 方向 | TaskId、WakeToken、Descriptor、Deadline、枚举 |
| 过度继承 | NonCopyable 继承、未使用的 enable_shared_from_this | 删除无领域意义的继承 |
| 重复表达 | Closure 与 Task；多个回调绑定重载 | 统一任务表达，调用方使用 lambda |
| 碎片封装 | IoEvent 无独立资源生命周期 | 改为 wait_ready 函数 |
| 层次污染 | Poller 接口含 Fiber/Timer；Mutex 直接向 owner 入队 | Reactor 只返回资源事件，Worker 独占调度转换 |
| 全局状态 | Runtime 单例、全局栈配置、fd 超时缓存 | 显式 Runtime 和操作级配置 |
| 重复状态 | 多份等待完成标志，WaitGroup 计数与事件，Channel 条件镜像 | 每项领域状态只有一个事实来源 |
| 错误模型 | bool、errno、空值、异常、日志后忽略混用 | 按错误性质建立一致策略 |
| 测试耦合 | 私有成员访问、全局启停、真实时间 | 验证协议与行为，注入事件和时间输入 |

Poller 已有测试替身，不能因生产只有一个实现就认定无价值。共享栈及快照缓冲池
有真实资源职责，不能仅因类小而删除。删除与合并必须依据职责，而非行数或名称。
本提案不删除 zco 范围外未经核实的死代码。

## 3. 问题分级与证据

P0 是首先必须解决的正确性或生命周期问题；P1 是主要架构与接口问题；
P2 是封装、测试和工程问题。静态风险与动态复现分别标注。
源码链接及行号以本文件记录的参照版本为准，实施后需要更新。

### P0-1：生命周期状态先于资源公布

来源：[runtime_manager.cc](../src/runtime_manager.cc#L129)，`Runtime::init/shutdown/submit_to`。

init 先将 started 置为 true，随后创建并启动 processors。并发提交可能观察到
“已启动”，却读取空或正在变化的 vector；submit_to 还对 vector 大小取模。
shutdown 先公布未启动，再停止、join 和清空，提交路径可能再次进入 init。

原子 bool 无法保护整个启动、访问和销毁协议。线程创建或分配异常缺少完整回滚；
工作线程异步启动失败也没有形成对调用者的可靠反馈。

- 证据：静态确认存在未统一同步的访问路径。
- 未验证：并发提交与启停压力、资源失败注入、自连接路径。
- 目标：构造完成和全部工作线程就绪后才能提交；Stopping 后提交明确失败。

### P0-2：丢失唤醒与重复入队

来源：[event.cc](../src/event.cc#L104)、
[runtime_manager.cc](../src/runtime_manager.cc#L379)、
[processor.cc](../src/processor.cc#L482)。

Event 在登记等待者并释放锁后才 prepare_current_wait。signal 可以先消费等待
资格，此时 Fiber 仍为 Running，resume 被忽略，随后协程才进入等待。

受控探针在登记与 prepare 之间触发 signal，得到：

```text
signal after registration, before prepare: received=0
```

有限等待实际返回超时；无限等待可能永久挂起是基于相同路径的推断。

resume_fiber 与 dispatch_resumed_fiber 都可能入队。探针模拟外部唤醒先于
切换收尾的合法状态交错，得到：

```text
ready entries after wake=1, after dispatch=2
```

这验证了重复队列项，不表示已经复现后续对象复用损坏或生产崩溃。
公开 resume(void*) 又没有绑定某一次等待，存在晚到恢复影响后续等待的风险。

- 证据：丢失唤醒、重复入队经受控探针验证。
- 目标：完成可先于挂起；等待代次明确；只有 Worker 可以转换调度状态并入队。

### P0-3：挂起协程自持有与关闭泄漏

来源：[fiber.cc](../src/fiber.cc#L28)、[event.cc](../src/event.cc#L104)。

协程入口在协程自己的栈上保存 shared_ptr<Fiber>，等待过程也保存强引用。
Processor 缺少独立拥有全部存活协程的表，外部句柄注册则按需发生。

探针让未导出外部句柄的协程无限等待，shutdown 后得到：

```text
parked fiber expired after shutdown=0
```

这验证了 Fiber 未释放。其非 owning Processor* 在运行时销毁后失效，进一步
访问的后果属于静态风险。完成的 Fiber 还未立即清空 task，捕获对象可能随对象池保留。

- 证据：挂起 Fiber 在关闭后保留已验证；未做完整泄漏与悬空访问检测。
- 目标：Worker 唯一拥有存活协程；等待和结果句柄不持有执行对象。

### P0-4：errno 与超时原因失真

来源：[processor.cc](../src/processor.cc)、
[io_wait_service.cc](../src/io_wait_service.cc#L114)、[io_event.cc](../src/io_event.cc)。

上下文切换没有隔离 errno。一个协程设置 EDOM 后 yield，另一个设置 EINVAL，
第一个恢复后观察到 EINVAL。

I/O 超时未可靠覆盖非阻塞 syscall 留下的 EAGAIN，IoEvent 只在 errno 为零时
设置 ETIMEDOUT。实际输出：

```text
errno after yield=22, expected=33
timed receive rc=-1, errno=11, timeout flag=1
```

znet TLS 等待已有补偿判断，底层错误细节因此影响上层控制流。

- 证据：两项均经探针验证。
- 目标：切换时保存恢复 errno；操作结果直接表达 timeout、canceled、closed 或 I/O 错误。

### P0-5：注册回滚与资源身份不完整

来源：[epoller.cc](../src/epoller.cc#L121)、
[io_wait_service.cc](../src/io_wait_service.cc#L36)、[hook.cc](../src/hook.cc#L403)。

组合读写注册先写入读等待者，再检查写冲突；冲突返回没有回滚已写的读状态。
探针结果：

```text
initial write registration=1
conflicting combined registration=0, errno=16
subsequent read-only registration=0, errno=16
```

另外，IoWaiter 发布到注册表后才赋值非原子 timer 字段，外部取消路径可能并发
读取。事件使用裸 fd 关联，缺少资源代次；dup2/dup3 替换目标 fd 与旧等待的
取消也没有完整协调。

- 证据：组合注册残留已验证。
- 静态风险：timer 字段并发访问、fd 复用和描述符替换；未做 TSAN 或压力复现。
- 目标：注册失败不留部分状态；资源身份和等待身份独立；关闭顺序受协议约束。

### P1-1：Runtime、Processor 和 Fiber 边界不清

Runtime 将配置、生命周期、调度、句柄和取消混合；Processor 将调度与资源实现
混合；Fiber 通过 owner 反向获得栈资源。应拆分变化原因，并删除跨层转发。
不按每个方法建立新类，也不以保留旧类为出发点。

### P1-2：抽象边界与价值不匹配

[Poller](../include/zco/internal/poller.h) 需要保留测试隔离价值，但应移除 Fiber、
TimerToken 和协程完成状态。IoEvent 改为函数；NonCopyable 继承删除；Fiber
未使用的 enable_shared_from_this 删除；Closure 和 Task 收敛为一种表达。
栈缓冲与快照缓存纳入执行模块内部，避免为了细分技术步骤形成转发链。

### P1-3：重复状态与等待协议分裂

一次等待涉及 Fiber.state、Fiber.timed_out、CoroutineWaiterEntry.active、
IoWaiter.active/error、TimerToken.cancelled。多个局部原子状态无法替代统一协议。

[Channel](../include/zco/channel.h) 用 not_empty/not_full Event 镜像队列条件；
done_ 保存所有调用者共享的最后操作结果。并发调用者无法用它判断自己的成功。

[WaitGroup](../src/wait_group.cc#L26) 的原子计数和 Event 分别修改，存在交错：

```text
add: 计数 0→1
done: 计数 1→0，signal
add: reset
最终：计数为 0，事件未触发
```

其注释允许 add/done 并发。该风险为静态推导，未单独动态复现。
目标是每个谓词只有一个事实来源，条件检查与等待登记原子完成。

### P1-4：同步原语跨层调度

[Mutex](../src/mutex.cc) 直接操作 Fiber 并访问 owner 入队；通用 resume 也没有
表达 Mutex 所有权已授予。同步对象应只维护领域状态和等待资格，调度转换归 Worker。

### P1-5：API 不表达生命周期、权限和预算

- go 隐式启动全局 Runtime；Scheduler* 不表达失效时间。
- 定向任务也进入可窃取队列，接口没有严格线程绑定语义。
- current_coroutine/resume(void*) 混合身份与恢复权限。
- Channel 输出参数、done、流运算符和 bool 转换表达不一致。
- Channel 重试使用完整相对超时，可能超过总预算。
- co_send/co_recvn 在失败时不能完整表达已经传输的字节数。
- co_shutdown(char)、整数无限超时哨兵承载隐含领域含义。

这些接口仍有调用者；删除属于主动 API 重设计，不是把它们误认为未使用代码。

### P1-6：hook 聚合网络策略、重复重试与全局缓存

[hook.cc](../src/hook.cc) 同时管理重试、fd 元数据、socket 超时、取消、TCP 选项和
简单 syscall 包装。缓存与操作系统状态重复，外部 close/dup/setsockopt 需要额外
同步。已有通用重试循环，但聚合发送、接收、连接和接受仍各自维护部分逻辑。

目标：网络创建、选项和连接策略归 znet；zco 管理资源就绪和协程 I/O。
使用一次操作的绝对截止时间，删除长期 fd 超时镜像缓存。

### P1-7：错误传播、日志与任务结果混合

[Fiber::run](../src/fiber.cc#L178) 捕获异常后记录日志并结束，提交者无法观察失败。
其他层混用 bool、errno、空值、异常和日志后忽略。下层传播错误，处理层记录日志；
任务异常保存到完成状态。协议、业务错误留在 znet/zhttp，不在 zco 引入空泛分类。

### P2：封装、测试与工程问题

- [安装规则](../../cmake/ZlynxPackage.cmake) 安装整个 include，旧内部头文件也暴露给下游。
- 部分测试通过 define private public 访问布局，难以随职责迁移。
- 全局 Runtime 使纯队列或缓冲测试也依赖启停；定时器测试依赖真实时间。
- [定时器基准](../tests/benchmark/zco_performance.cc#L389) 使用 sleep_for(0)，主要测 yield。
- [基准脚本](../tests/benchmark/zco_perf.sh) 含旧目标名和构建选项，需同步迁移。
- Breaking Changes 需要更新安装接口、显式依赖和 ABI 版本。

## 4. 目标模块与依赖

| 边界 | 职责 | 不承担的职责 |
|---|---|---|
| 调度 | 提交、任务分配、窃取、就绪队列、任务状态 | 栈缓存细节、网络策略 |
| 执行 | 原生上下文、独立栈、共享栈、快照、errno 隔离 | 全局 Runtime、I/O、等待完成原因 |
| 等待 | 一次性完成、截止时间、取消、等待登记 | 查找或直接访问具体 Worker |
| 描述符 I/O | 资源生命周期、注册、就绪和传输结果 | Fiber、业务协议、TCP 配置 |
| 同步 | 事件、互斥、计数、队列的领域状态 | 就绪队列、epoll、具体协程实现 |

```text
应用入口 / znet / zhttp
    ├── Runtime / Executor / TaskHandle
    ├── 同步原语 ───────────────→ 等待协议
    └── 描述符 I/O ─────────────→ 等待协议

Runtime 组装实现
    ├── 创建任务队列和工作线程
    ├── 创建执行资源
    └── 注入 Reactor 与完成通知端点

Worker → 执行模块
Worker → 等待协议
Worker → Reactor 接口
LinuxEpollReactor → Reactor 接口
```

组装入口可以创建具体实现；其余模块只能依赖所需能力。
等待完成通过注入的窄通知能力提交，不反向查找 Runtime 或修改 Worker。
依赖检查需要区分组装关系与模块接口，不能通过把双向依赖藏进回调来回避审查：
回调载荷仅为等待身份和完成通知，不暴露 Worker、Fiber 或任意服务访问能力。

保留两个有实际依据的隔离点：

1. Reactor：内核 I/O 与已有测试替身需求。
2. 完成通知端点：跨线程完成与线程内调度转换的边界。

平台上下文使用私有函数；时间作为参数输入。无需虚拟 Clock、通用工厂、
ManagerRegistry、ServiceLocator 或为每种系统调用建立策略类。

### 4.1 Runtime 与任务调度

建议 API 形状如下，属于设计示意，当前不可直接编译：

```cpp
Runtime runtime(options);           // 全部资源就绪后构造成功
auto task = runtime.spawn(fn);      // 上下文创建前允许窃取
auto executor = runtime.executor(0);
auto pinned = executor.spawn(fn);   // 绑定该工作线程

task.join();
runtime.request_stop();
runtime.join();
```

- RuntimeOptions 构造后不可变，不允许共享实例被某个服务器重新配置。
- 构造资源失败完整回滚；就绪前不公布提交入口。
- Executor 是失效可检查的非 owning 端点，不是 Scheduler 裸指针。
- 普通任务仅在创建上下文前可迁移；绑定任务不参与窃取。
- Worker 独占全部存活任务和 Continuation；外部线程只能提交命令。
- TaskHandle 保存完成结果，可在 Runtime 销毁后查询，不持有 Fiber。
- 不再提供自动启动、全局重启或旧 go adapter。

### 4.2 执行与栈

Continuation 只负责执行上下文和栈关联，不保存 Processor*、等待结果和全局句柄。
调度状态属于 Worker 的任务记录。StackArena 管理工作线程的共享槽、独立栈及
快照缓冲，通过 RAII 释放。

原生上下文函数必须检查系统调用结果，并在每次切换时保存、恢复 errno。
共享栈需明确“保存旧使用者后才覆盖栈”的顺序与快照范围不变量。
不支持的平台或无效栈配置应明确失败，不记录警告后继续执行。

删除 Fiber 对象池的身份复用。缓冲缓存有独立资源价值，但容量策略必须有测量依据，
不因为旧实现存在就原样保留。任务完成时立即释放回调捕获，再公布完成结果。

### 4.3 等待协议与时间

核心值为 WaitId、WaitOutcome、Deadline。WakeToken 只能完成一次具体等待，
携带等待代次，过期通知不影响后续等待。

```text
在谓词保护下登记等待
    ↓
完成原因可以先被记录
    ↓
Worker 检查已完成则不挂起，否则挂起
    ↓
就绪 / 超时 / 取消竞争同一完成状态
    ↓
提交完成通知
    ↓
Worker 消费通知，最多入队一次
    ↓
撤销定时器与资源注册
```

必须覆盖已完成但尚未挂起、正在切换、已经挂起三种窗口。
注册对象用 RAII 统一清理；定时器不强持有 Fiber，取消应及时释放捕获。

TimerQueue 接收绝对 Deadline，expire(now) 和 next_deadline() 使用显式时间输入。
真实时间只在事件循环或 API 边界读取，不为测试增加全局模拟时间。
一次调用只计算一次截止时间，重试不得重置总预算。

普通线程使用条件变量，协程使用挂起端点；两者共享完成结果和谓词保护语义。
允许私有 TLS 表达当前执行入口，但不能借它查找任意资源或全局服务。

### 4.4 描述符与 Reactor

Descriptor 是 move-only fd 拥有者，明确采用和转移所有权。借用接口不转移所有权。
对 TLS 等外部库提供有约束的 native_handle 借用；外部不得绕过生命周期协议关闭。

区分三种身份：

| 身份 | 含义 |
|---|---|
| 原生 fd | 可复用的操作系统句柄 |
| ResourceId | 资源身份与代次 |
| RegistrationId | 一次就绪注册 |

Reactor 仅注册资源兴趣、返回 ReadyEvent，并提供阻塞与唤醒能力；
Linux 实现通过 RAII 管理 epoll/eventfd。接口不包含 Fiber、TimerToken 或业务回调。
注册失败不得留下部分修改。

外部关闭交给所属端点：先撤销注册和完成等待，再关闭原生 fd。
析构可以把唯一 fd 所有权移交关闭命令，避免要求析构线程阻塞等待；运行时已经
完全停止时才直接 native close。具体实现必须验证销毁与端点失效的竞争。

操作使用 Deadline 和类型化结果。需要沿用 socket 默认超时时，显式选择并在
操作开始时查询，避免永久缓存。TCP/UDP 创建、选项、connect/accept 策略归 znet。
zco 保留描述符就绪、基础读取写入和流传输能力；整个项目保留原有网络能力。

### 4.5 同步原语

保留有领域价值的 Event、Mutex、WaitGroup、Channel，不按每个步骤拆类。
内部等待队列处理混合线程等待票据，不暴露 Fiber 或具体 Worker。

- Event：拥有触发和复位状态，明确保留手动/自动复位语义。
- Mutex：拥有互斥状态和授予顺序，唤醒必须对应实际授予。
- WaitGroup：计数是归零条件的唯一事实来源。
- Channel：队列、容量和关闭状态是唯一事实来源，删除条件 Event 镜像。

接口示意：

```cpp
Result<T> Channel<T>::receive(Deadline);
Result<void> Channel<T>::send(T, Deadline);
```

每次调用返回自己的结果，支持 move-only 值。删除 done、忽略结果的流运算符及
含糊 bool 转换。修改状态的接口自然表达可变性，不用全字段 mutable 隐藏修改。
复制共享句柄可以保留，但必须明确表示共享状态，而不是无意复制同步对象。

## 5. 生命周期与所有权

| 对象 | 创建与拥有 | 销毁、共享和空值规则 |
|---|---|---|
| Runtime | 应用创建，栈对象或 unique_ptr | 外部线程最终 join；有效对象拥有完整资源 |
| Worker | Runtime 的 unique_ptr | join 后销毁，不向外暴露 owning 指针 |
| 任务记录、Continuation | Worker 唯一拥有 | 完成清理后销毁；等待者不拥有它们 |
| StackArena | Worker 唯一拥有 | 管理该线程所有栈资源，线程退出后释放 |
| TaskHandle | 共享完成状态 | 可长于 Runtime；不延长 Worker 生命周期 |
| Executor | 弱关联提交端点 | 允许失效，提交返回明确结果 |
| WakeToken | 弱关联一次等待和通知端点 | 过期或重复完成无效，不拥有 Fiber |
| Descriptor | 唯一拥有 fd | move 转移；关闭命令可接管其唯一所有权 |
| 同步状态 | 值、unique_ptr 或明确共享的句柄状态 | 仅在真实共享时用 shared_ptr |

不因“安全”默认使用 shared_ptr。非 owning 的线程内依赖优先使用引用，跨线程
可能失效的端点采用可验证弱关联。运行时代次、任务身份和等待代次不得混为一个整数。

停止流程：

```text
Running → Stopping：拒绝新提交
    ↓
未执行任务收到取消结果
    ↓
挂起等待完成取消，任务恢复并清理
    ↓
工作线程退出
    ↓
外部 join
    ↓
销毁资源，进入 Stopped
```

request_stop 可以从工作线程调用；join 必须避免自连接。
析构执行停止与 join，用户必须保证其所有者在适合 join 的线程销毁。

协作式运行时无法强制终止永不让出执行权的用户代码。取消在等待和让出点生效，
用户持续忽略取消也会阻止退出。不能释放活动栈来实现快速关闭；该限制必须写入 API 契约。
需要新配置时创建新的 Runtime，不通过重置单例恢复默认值。

## 6. 错误与日志策略

| 错误性质 | 建议策略 |
|---|---|
| 编程错误 | 明确契约，assert 或 logic_error；不静默忽略 |
| 无效配置 | 构造时 invalid_argument，尚未发布资源 |
| 启动资源失败 | system_error，构造完整回滚 |
| 超时、取消、关闭、I/O 失败 | 类型化 Result 或具体操作结果，带 error_code 和操作上下文 |
| 用户任务异常 | exception_ptr 写入任务完成状态，由 TaskHandle 观察 |
| 协议、业务错误 | 在 znet/zhttp 领域定义，zco 不引入未使用分类 |

继续使用 C++14，不直接依赖 std::expected。Result 只表达项目实际需要的成功或
失败值，不建立通用错误框架。聚合传输结果应保留 bytes、error 和 eof，失败时也
能报告部分进度；区分 write_some/write_all、read_some/read_exact。

底层传播错误，处理层负责日志，避免每层重复记录。移除核心公共头文件对 zlog
的依赖，znet/zhttp 显式链接自己的日志依赖。未观察任务异常的处理策略必须明确，
不能在所有层默认打印后吞掉；也不为此引入未要求的 detached task 框架。

## 7. 完整目标目录

下面是实施目标，不是当前目录。私有头文件不安装，detail/wait_queue.h 仅提供
Channel 模板所需的最小桥接声明，不重新暴露整个内部实现。

```text
zco/
├── CMakeLists.txt
├── README.md
├── docs/
│   └── architecture-redesign.md
├── include/
│   └── zco/
│       ├── zco.h
│       ├── runtime.h
│       ├── coroutine.h
│       ├── deadline.h
│       ├── result.h
│       ├── io/
│       │   ├── descriptor.h
│       │   └── operations.h
│       ├── sync/
│       │   ├── event.h
│       │   ├── mutex.h
│       │   ├── wait_group.h
│       │   └── channel.h
│       └── detail/
│           └── wait_queue.h
├── src/
│   ├── runtime/
│   │   ├── runtime.cc
│   │   ├── coroutine.cc
│   │   ├── worker.h
│   │   ├── worker.cc
│   │   ├── task_queues.h
│   │   └── task_queues.cc
│   ├── execution/
│   │   ├── continuation.h
│   │   ├── continuation.cc
│   │   ├── stack_arena.h
│   │   ├── stack_arena.cc
│   │   └── linux/
│   │       ├── native_context.h
│   │       └── native_context.cc
│   ├── wait/
│   │   ├── wait_state.h
│   │   ├── wait_state.cc
│   │   ├── wait_queue.cc
│   │   ├── timer_queue.h
│   │   └── timer_queue.cc
│   ├── io/
│   │   ├── descriptor.cc
│   │   ├── operations.cc
│   │   ├── reactor.h
│   │   └── linux/
│   │       ├── epoll_reactor.h
│   │       └── epoll_reactor.cc
│   └── sync/
│       ├── event.cc
│       ├── mutex.cc
│       └── wait_group.cc
└── tests/
    ├── CMakeLists.txt
    ├── support/
    │   ├── fake_reactor.h
    │   └── runtime_fixture.h
    ├── unit/
    │   ├── runtime_lifecycle_test.cc
    │   ├── task_scheduling_test.cc
    │   ├── wait_state_test.cc
    │   ├── timer_queue_test.cc
    │   ├── stack_arena_test.cc
    │   ├── synchronization_test.cc
    │   └── io_operations_test.cc
    ├── integration/
    │   ├── runtime_shutdown_test.cc
    │   ├── descriptor_lifecycle_test.cc
    │   └── socket_io_test.cc
    └── benchmark/
        ├── zco_performance.cc
        └── zco_perf.sh
```

## 8. 替换、合并与删除清单

### 8.1 类型和职责迁移

| 旧类型或模块 | 目标处理 |
|---|---|
| 全局 Runtime、Scheduler | 显式 Runtime、失效可检查的 Executor；配置归实例 |
| Processor | Worker；栈、等待、I/O 迁到对应模块 |
| Fiber、Context | Continuation 和私有原生上下文函数；状态归任务记录 |
| FiberStackManager、SharedStackBuffer、SnapshotBufferPool | StackArena 内部统一管理，不公开池转发接口 |
| FiberPool | 删除身份复用；缓冲缓存另按测量决定 |
| FiberHandleRegistry | 删除；身份、完成状态和等待权限分别表达 |
| StealQueue | TaskQueues 区分绑定任务和可窃取任务 |
| CoroutineWaiterEntry、IoWaiter | 统一 WaitState、等待票据和注册协议 |
| IoWaitService | I/O 编排使用统一等待协议 |
| Poller、Epoller | 窄 Reactor 接口及 Linux 实现 |
| IoEvent | 删除，改为 wait_ready 函数 |
| Closure | 删除，统一 Task 和 lambda 捕获 |
| NonCopyable | 删除，类型直接声明删除复制操作 |
| Fiber 的 enable_shared_from_this | 删除未使用继承 |
| TimerQueue | 保留计时职责，显式输入时间，解除强捕获 |
| Event、Mutex、WaitGroup、Channel | 保留领域对象，替换等待协议和结果接口 |
| zco_logger | 删除核心日志入口，错误处理层负责记录 |

### 8.2 文件处理

删除旧内部头文件目录，其职责全部迁到 src 私有模块：

```text
zco/include/zco/internal/
  context.h
  coroutine_waiter.h
  epoller.h
  fiber.h
  fiber_handle_registry.h
  fiber_pool.h
  fiber_stack_manager.h
  io_wait_service.h
  noncopyable.h
  poller.h
  processor.h
  runtime_manager.h
  shared_stack_buffer.h
  snapshot_buffer_pool.h
  steal_queue.h
  timer.h
```

删除旧公共入口：

```text
zco/include/zco/sched.h
zco/include/zco/hook.h
zco/include/zco/io_event.h
zco/include/zco/zco_logger.h
```

event.h、mutex.h、wait_group.h、channel.h 迁到 sync 子目录；zco.h 重写，不保留
旧 include 的转发兼容文件。

删除并由新模块替换旧实现：

```text
zco/src/runtime_manager.cc
zco/src/processor.cc
zco/src/sched.cc
zco/src/fiber.cc
zco/src/context.cc
zco/src/fiber_stack_manager.cc
zco/src/shared_stack_buffer.cc
zco/src/snapshot_buffer_pool.cc
zco/src/fiber_pool.cc
zco/src/fiber_handle_registry.cc
zco/src/steal_queue.cc
zco/src/io_wait_service.cc
zco/src/epoller.cc
zco/src/io_event.cc
zco/src/hook.cc
zco/src/zco_logger.cc
```

旧 timer.cc 的计时职责迁入 wait，event.cc、mutex.cc、wait_group.cc 迁入 sync，
移除旧位置，避免两套实现。

旧 noncopyable、fiber_pool、fiber_handle_registry、coroutine_waiter 等类型专属
测试随接口删除；其有效行为覆盖迁到新模块。上下文、栈、Poller、I/O、调度测试
按新边界重写，不能以删除旧类型为理由删除能力验证。

### 8.3 函数、接口及兼容层

计划删除或替换：

- 全局 init/shutdown/go、co_stack_num/size/model：替换为 Runtime 实例操作。
- main_sched/next_sched、Scheduler*：替换为 Executor。
- current_coroutine/resume(void*)：替换为身份查询和单次 WakeToken。
- Closure 提交及重复参数绑定重载：统一 Task，由调用方 lambda 表达。
- Channel 的 bool 输出参数接口、done、吞结果的流运算符及含糊 bool 转换。
- 全局 fd 超时元数据及手工同步入口。
- zco 公共日志初始化与 getter 入口。
- co_socket/tcp_socket/udp_socket、bind/listen、socket 选项等简单网络包装：职责迁入 znet。
- co_connect/accept、UDP 操作和 TCP 关闭策略：在 znet 使用新的就绪等待能力。
- 聚合 read/write/send/recvn：迁为保留部分进度、EOF 和错误的自然接口。
- co_close 的延迟策略、co_reset_tcp_socket、char shutdown 方向：网络策略归 znet，方向使用枚举。
- 伞形头文件中的旧别名和历史入口：不保留兼容 adapter。

上述迁移已实施，仓库内调用方已迁移，旧接口及实现已删除；最终目录和验证结果见
[重构验证记录](refactor-validation.md)。

## 9. Breaking Changes 与调用方

| 方面 | 变化及影响 |
|---|---|
| API | 显式提交、单次唤醒、类型化等待与传输结果；所有调用方需要迁移 |
| namespace | 保留 zco 根空间，I/O 与协程操作使用清晰子边界；私有类型不公开 |
| include | 删除 sched/hook/io_event/zco_logger 入口，同步头文件迁目录 |
| directory | 按能力组织，旧 internal 不再安装 |
| constructor | Runtime 接收不可变配置；服务器和连接显式接收所需执行端点 |
| ownership | Worker 独占执行对象；TaskHandle 只共享完成状态；Descriptor 唯一拥有 fd |
| configuration | 全局启动前设置改为实例选项；没有隐式启动和全局重置 |
| behavior | 定向任务严格绑定；超时原因准确；取消、任务失败和部分进度可观察 |
| ABI / package | 更新公共符号、安装接口与 ABI 主版本，重新验证包消费 |
| logging | znet/zhttp 显式声明 zlog 依赖，不依赖 zco 转发 |

主要调用方：

| 位置 | 迁移要求 |
|---|---|
| [znet/src/tcp_server.cc](../../znet/src/tcp_server.cc) | 移除服务器内部全局 init；注入 Runtime 或 Executor，不停止其他服务器资源 |
| [znet/src/acceptor.cc](../../znet/src/acceptor.cc) | 使用明确的执行端点提交接受任务 |
| [znet/src/connection_actor.cc](../../znet/src/connection_actor.cc) | Scheduler* 改 Executor，协程身份改 TaskId，删除全局回退 |
| [connection_actor.h](../../znet/include/znet/internal/connection_actor.h) | 更新非 owning 端点与身份字段，明确构造契约 |
| [znet/src/tcp_connection.cc](../../znet/src/tcp_connection.cc) | Descriptor 组合、明确等待结果、删除 TLS errno 补偿 |
| [znet/src/socket.cc](../../znet/src/socket.cc)、[buffer.cc](../../znet/src/buffer.cc) | 网络策略、fd 唯一所有权和 I/O 结果迁移 |
| [znet/src/znet_logger.cc](../../znet/src/znet_logger.cc) | 移除依赖 zco 初始化日志的链条 |
| [zhttp/runtime/server_bootstrap.cc](../../zhttp/runtime/server_bootstrap.cc) | 应用入口组装 RuntimeOptions 与 Runtime，不改进程级配置 |
| [zhttp/server_config.h](../../zhttp/server_config.h) | 配置映射到实例，明确多服务器共享或独立 Runtime |
| CMake、README、测试、基准 | 更新链接、包依赖、示例及旧 API 引用 |

当多个服务器共享 Runtime 时由应用明确共享并统一管理生命周期；不通过默认
单例隐式共享，也不由某个 TcpServer 决定整个进程的停止。

## 10. 实施顺序与阶段出口

以下为实施顺序。具体完成记录见验证文档；迁移需保持可编译、可验证，避免长期
同时保留新旧路径，不为了兼容保留错误接口。

| 阶段 | 工作 | 出口条件 |
|---|---|---|
| 1 | 固定任务身份、生命周期、等待与入队不变量；建立确定性交错测试 | 新协议能验证关键窗口，复现 P0 问题 |
| 2 | 显式 Runtime、提交端点、Worker 存活任务所有权 | 启停、异常回滚、停止竞争与资源释放测试通过 |
| 3 | 统一等待、Deadline、TimerQueue、四种同步原语 | 所有完成竞争最多一次，无丢失唤醒及预算重置 |
| 4 | Descriptor、Reactor、类型化 I/O 结果 | 注册回滚、fd 代次、关闭、部分进度测试通过 |
| 5 | Continuation、StackArena、原生上下文函数 | 两种栈模型、errno、捕获释放及上下文失败验证通过 |
| 6 | 迁移 znet/zhttp、安装接口、日志和基准，删除旧文件 | 全仓库无旧生产 API 引用，无两套实现 |
| 7 | 全项目与包消费验证，重测基准 | 验收矩阵完成，剩余风险明确记录 |

若阶段之间需要过渡，必须限制在当前实施分支内并在交付前删除；不能把临时
adapter 或 TODO 当作最终架构。资源缓存和负载策略优化安排在正确性验证之后。

## 11. 验证记录与验收矩阵

### 11.1 分析阶段已执行

```bash
cmake --build build/debug -j 4
ZCO_TEST_LOG_LEVEL=error ctest --test-dir build/debug -R '^zco\.' --output-on-failure -j 4
ZCO_TEST_LOG_LEVEL=error ctest --test-dir build/debug --output-on-failure -j 4
```

| 检查 | 结果 |
|---|---|
| 当前 Debug 构建 | 成功；使用已有配置，不是全新干净构建 |
| zco CTest | 30/30 个目标通过 |
| 全项目 CTest | 93/93 个目标通过 |
| Event 登记与 prepare 窗口 | 丢失唤醒已复现 |
| 唤醒与切换收尾交错 | 重复就绪队列项已复现 |
| yield 前后 errno | 覆盖已复现 |
| I/O 超时 | EAGAIN 与 timeout 标志同时出现已复现 |
| shutdown 后挂起 Fiber | 未释放已复现 |
| epoll 组合注册失败 | 残留注册状态已复现 |

探针当时位于 /tmp，未提交为仓库测试：zco_architecture_probe.cc、
zco_event_interleaving_probe.cc、zco_epoll_registration_probe.cc。
部分探针使用内部访问或链接包装控制交错，仅用于诊断，不作为新测试架构。
这些记录不等同于随机压力测试；临时文件不是可长期依赖的证据存储。
实施前应将其行为转为持久、确定性的回归测试。

现有测试通过不能排除已复现缺陷，也不能证明目标架构已完成。

### 11.2 必须新增或重写的验证

| 范围 | 场景 | 验收要求 |
|---|---|---|
| 等待交错 | 登记前、登记后、挂起前、切换中、挂起后完成 | 不丢失完成，最多入队一次 |
| 完成竞争 | signal、timeout、close、shutdown 同时到达 | 一个终态，其余完成无效 |
| 等待代次 | 旧 WakeToken 到达同一任务的新等待 | 不能完成新等待 |
| Mutex | 唤醒与锁授予、取消竞争 | 未获得锁不能返回成功 |
| 生命周期 | 提交与停止竞争、启动失败、重复停止 | 确定结果，无半启动资源和自连接 |
| 停止清理 | Event、Mutex、Channel、I/O、Timer 无限等待 | 取消后正常清理，捕获和执行资源释放 |
| 任务结果 | 正常完成、异常、尚未执行即取消 | TaskHandle 可观察结果，释放 task 捕获 |
| 调度 | 普通任务窃取、绑定任务、上下文创建后恢复 | 绑定任务不窃取，执行上下文不迁移 |
| 栈 | 共享槽复用、嵌套调用、独立栈、无效配置 | 快照正确、切换顺序正确、失败明确 |
| errno | 多协程交错并反复 yield/wait | 各协程恢复自己的 errno |
| Reactor | 组合注册冲突、epoll_ctl 失败、取消注册 | 无部分状态与悬空注册 |
| 资源代次 | fd 复用、旧事件、dup2/dup3 替换 | 旧事件不影响新资源 |
| I/O | 部分读写、EOF、总 Deadline、socket 默认超时 | 进度保留，错误准确，预算不重置 |
| 数据报 | 零长度发送接收、地址结果 | 保留数据报语义 |
| 同步状态 | WaitGroup add/done、Channel 并发与关闭 | 只有一份谓词，结果属于本次调用 |
| 时间 | 到期、取消、相同截止时间、大时间跳变 | 使用显式 now，无 wall-clock sleep 依赖 |
| 包消费 | 安装后 find_package、公开头文件、依赖 | 不暴露私有实现，所需依赖完整 |
| 全项目 | Debug/Release、独立 zco、allocator 开关 | 编译、测试及集成通过 |

可使用 Sanitizer 和压力测试补充，但需识别 ucontext/共享栈相关工具限制，
不能把工具不支持的结果当作无缺陷证据。

### 11.3 性能验证要求与分析阶段记录

先修正定时器基准，使其实际登记并触发 TimerQueue，而不是 sleep_for(0)。
在相同硬件、编译参数、线程数、任务量和栈配置下对比：

- 提交、yield、Channel、I/O 与真实定时等待吞吐。
- 延迟分布和尾延迟。
- RSS、分配次数、快照复制量和关闭时资源释放。

分析阶段未运行性能基线。实施后的同机对比见验证记录，提交及部分 I/O 场景存在
性能下降，不能承诺新架构更快。若代码量明显增加但理解成本、
依赖或测试复杂度未下降，应重新评估拆分，删除不必要抽象。

### 11.4 分析阶段未验证内容（历史记录）

以下项目在编写提案时未验证；实施后的完成状态与剩余限制见验证记录。

- 并发生命周期压力和启动失败注入。
- timer 字段竞争、fd 复用及描述符替换的动态检测。
- ASAN/TSAN 等完整检测与其他架构平台。
- 全新 Release 构建、独立安装消费和性能基线。
- 目标架构的编译和测试：尚未实施，不存在已通过结论。

## 12. 交付前架构复核

- [x] 每个模块围绕一个明确目标，没有新的 Manager/Helper/Util 杂物箱。
- [x] Worker 之外没有代码修改调度状态或直接向就绪队列投递 Fiber。
- [x] 没有全局资源查找、隐式启动及具体实现反向污染。
- [x] 每次等待只有一个终态，旧等待和旧资源事件不能影响新操作。
- [x] 每个核心对象的创建、拥有、销毁和失效条件均可从 API 看出。
- [x] 没有协程执行对象自持有、对象身份复用或完成回调捕获保留。
- [x] 抽象有测试隔离或生命周期理由，没有为假想扩展设计接口。
- [x] 同步状态和错误原因没有重复可变副本。
- [x] 旧文件、入口、转发兼容层及仅服务旧架构的代码已经删除。
- [x] 全部调用方、构建包、文档和基准完成迁移。
- [x] 验收矩阵有实际执行记录，静态推断与未验证风险仍明确标注。
