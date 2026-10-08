# zco

`zco` 是 Linux/C++14 有栈协程运行时，提供多线程调度、任务窃取、共享栈与独立栈、
定时等待、描述符 I/O，以及线程和协程混用的同步原语。2.0 使用显式 Runtime，
不再提供全局启动、隐式重启或旧 API 兼容层。

[架构重设计文档](docs/architecture-redesign.md) 保留问题证据和设计依据；
[重构验证记录](docs/refactor-validation.md) 记录实际实现、验证与限制。

## 快速开始

```cpp
#include "zco/zco.h"
#include <iostream>

int main() {
    zco::RuntimeOptions options;
    options.worker_count = 2;
    options.stack_model = zco::StackModel::kIndependent;
    options.stack_size = 64 * 1024;
    zco::Runtime runtime(options);
    zco::Channel<int> channel(8);

    auto producer = runtime.spawn([&] {
        channel.send(42).value();
        channel.close();
    });
    auto consumer = runtime.executor(0).spawn([&] {
        auto value = channel.receive();
        if (value) std::cout << value.value() << '\n';
    });
    producer.value().join().value();
    consumer.value().join().value();
    runtime.request_stop();
    runtime.join();
}
```

`spawn()` 返回 `Result<TaskHandle>`，失效或停止的端点返回取消错误。
普通任务在创建上下文前可被窃取；`Executor::spawn()` 严格绑定指定工作线程。
TaskHandle 只保存完成结果，可在 Runtime 销毁后查询。`join()` 会重新抛出用户任务
异常；未观察的异常保留在完成状态中，不自动记录日志。

Runtime 构造成功时资源和所有工作线程均已就绪。配置属于实例，构造后不可修改。
`request_stop()` 可从工作线程调用，拒绝新提交、取消未执行任务并完成挂起等待。
`join()` 等待工作线程退出，须先请求停止，且不得从该 Runtime 的工作线程调用。
析构自动停止并 join；Runtime 的所有者必须在适合 join 的外部线程销毁。

取消是协作式的：yield 在停止时展开用户栈，等待返回取消结果。永不让出执行权、
或持续忽略取消的用户代码会阻止退出；运行时不会释放仍在执行的栈来强制关闭。
同步对象、借用的缓冲和业务对象必须活到相关操作结束；销毁它们前应等待任务完成，
或先停止并 join Runtime。共享栈中的局部地址不能交给其他协程或线程长期使用。

## 等待与同步

所有等待共享单次完成协议，完成原因包含 ready、timeout、canceled、closed。
旧 WakeToken 无法影响后续等待。`Deadline::after(duration)` 在操作开始时计算绝对
截止时间，重试沿用同一预算；默认 Deadline 表示无限等待。

- `Event` 支持自动重置和手动重置，`wait()` 返回本次等待的 `Result<void>`。
- `Mutex::lock()` 返回结果；只有实际获得锁才成功，成功后必须调用 `unlock()`。
- `WaitGroup` 在同一保护锁下修改计数和登记等待，计数下溢是编程错误。
- `Channel<T>` 是不可复制的有界队列，支持 move-only 值；`send()` 和 `receive()`
  返回各自的结果。关闭后拒绝发送，接收方可继续取出已有数据。

普通线程使用条件变量，协程使用挂起入口。核心等待和定时队列接受显式事件或时间
输入，测试无需启动全局运行时或依赖真实网络。

## 描述符 I/O

`zco::io::Descriptor(fd)` 接管 fd 的唯一所有权；使用协程 I/O 时 fd 必须非阻塞。
Descriptor 支持 move、duplicate 和替换为新副本。关闭先撤销注册、完成等待，再
关闭 fd；资源身份、注册身份和原生 fd 分开，旧事件不能唤醒新资源的等待。

`native_handle()` 仅供有生命周期约束的借用，例如 TLS 库；不得直接 close，或通过
dup2/dup3 覆盖借用的 fd。短暂原生系统调用可使用 `borrow()` 保持关闭同步；必须先
释放 Borrow，再等待或让出。拥有者的 move/替换需串行执行，已开始的 I/O 始终绑定
原资源；线程关闭和替换可取消该资源正在挂起的等待。

`wait_ready()` 只返回就绪结果。`read_some`/`write_some` 和 `read_exact`/`write_all`
分别表达单次与聚合传输，返回 `TransferResult { bytes, error, eof }`，失败仍保留部分
进度。默认 Deadline 不读取 socket 选项；显式调用 `socket_deadline()` 时，操作开始
查询内核默认超时并返回 `Result<Deadline>`，没有长期 fd 元数据缓存。
TCP/UDP 创建、选项、接受和连接策略属于 znet。

## 目录与构建

公共头文件位于 `include/zco`；私有实现位于 `src/runtime`、`src/execution`、
`src/wait`、`src/io`、`src/sync`，不安装。`detail/wait_queue.h` 只包含 Channel
模板需要的桥接声明。核心只链接 Threads，不依赖 zlog；znet/zhttp 自行声明日志依赖。
ABI 主版本为 2。

StackArena 为每个工作线程最多缓存一块已结束任务的独立栈，避免连续任务反复映射；
缓存不含上下文、回调、任务身份或等待状态，join 时释放。这个容量来自本机提交基准
验证，未扩展为可调池。

```bash
cmake --preset debug
cmake --build --preset debug -j 4
ctest --test-dir build/debug -R '^zco\.' --output-on-failure -j 4

# 独立构建，无需先安装 zlog。
cmake -S zco -B build/zco-standalone -G Ninja -DBUILD_TESTING=ON
cmake --build build/zco-standalone -j 4
ctest --test-dir build/zco-standalone --output-on-failure
```

安装消费：

```cmake
find_package(zco 2 CONFIG REQUIRED)
add_executable(demo main.cc)
target_link_libraries(demo PRIVATE zco::zco)
```

基准覆盖提交、yield、Channel、I/O 和真实登记并触发的定时等待，分别测试两种栈
模型，同时输出 timer 的延迟分位数、C++ new 分配次数、快照复制字节
以及停止前后 RSS。计数仅在基准专用静态库启用，生产库不携带这些统计：

```bash
cmake --preset perf
cmake --build --preset perf --target zco_performance -j 4
ZCO_PERF_SCALE_PCT=10 build/perf/zco/tests/zco_performance
BIN=build/perf/zco/tests/zco_performance zco/tests/benchmark/zco_perf.sh baseline
```

基准不纳入 CTest。不同机器、配置和任务预算的结果不可直接比较；验证记录不承诺
相对旧实现的性能提升。
