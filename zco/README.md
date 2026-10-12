# zco

zco 是 Linux/C++17 有栈协程运行时，提供显式 Runtime、多线程调度、任务窃取、
共享栈与独立栈、定时等待、同步原语和描述符 I/O。外部依赖为 Threads。

## 最小示例

```cpp
#include "zco/zco.h"
#include <iostream>

int main() {
    zco::Channel<int> channel(8);
    zco::RuntimeOptions options;
    options.worker_count = 2;
    options.stack_model = zco::StackModel::kIndependent;
    zco::Runtime runtime(options);

    auto producer = runtime.spawn([&] {
        channel.send(42).value();
        channel.close();
    });
    auto consumer = runtime.spawn([&] {
        std::cout << channel.receive().value() << '\n';
    });

    producer.value().join().value();
    consumer.value().join().value();
    runtime.request_stop();
    runtime.join();
    return 0;
}
```

`spawn()` 返回 `Result<TaskHandle>`，`TaskHandle::join()` 等待完成并观察任务异常。
`Runtime::spawn()` 提交的任务在首次执行前可被窃取；`runtime.executor(index).spawn()`
把任务绑定到指定工作线程。完成句柄可在 Runtime 销毁后查询。

## 生命周期与等待

Runtime 构造时启动工作线程，配置在构造后不可修改。`request_stop()` 拒绝新任务，
取消尚未执行的任务并唤醒挂起等待；`join()` 必须在该 Runtime 的工作线程之外调用，
且应先请求停止。析构会自动停止并等待退出。

取消是协作式的。用户任务需在让出或等待处响应取消；不让出执行权或持续忽略取消
会阻止运行时退出。同步对象、业务对象和借用缓冲必须活到相关操作结束。
共享栈中的局部地址不能交给其他协程或线程跨挂起长期使用。

`Deadline::after(duration)` 计算绝对截止时间，默认 Deadline 表示无限等待。
等待结果区分就绪、超时、取消和关闭，重试沿用同一截止时间。

| 接口 | 行为 |
| --- | --- |
| `yield()`、`sleep_for()`、`sleep_until()` | 主动让出及定时等待 |
| `Event` | 自动或手动重置事件 |
| `Mutex` | 获得锁后需显式 `unlock()`；获取失败返回结果 |
| `WaitGroup` | 计数与完成等待 |
| `Channel<T>` | 有界队列；关闭后拒绝发送，可继续取出已有数据 |

同步原语可用于普通线程和协程；普通线程等待使用条件变量，协程等待挂起任务。

## 描述符 I/O

`zco::io::Descriptor(fd)` 接管 fd 的唯一所有权，支持移动、复制底层描述符和关闭。
协程 I/O 要求 fd 在接管时为非阻塞，管理期间不得修改它及共享文件状态副本的
`O_NONBLOCK`。`native_handle()` 只提供借用，不能直接关闭或用 `dup2/dup3` 覆盖。

`wait_ready()` 等待就绪；`read_some()` / `write_some()` 表达一次传输，
`read_exact()` / `write_all()` 聚合多次传输。返回值包含字节进度、错误和 EOF，
失败时仍需处理已经传输的字节。关闭 Descriptor 会撤销注册并唤醒相关等待。

接口不会隐式读取 socket 超时选项；需要时显式调用 `socket_deadline()`。
这是显式 I/O API，调用方通过 [operations.h](include/zco/io/operations.h) 接入。

## 构建、测试与安装

以下命令从 zlynx 根目录执行，需要 Linux、C++17、CMake 3.18+；默认测试需要 GTest：

```bash
cmake -S zco -B build/zco -DCMAKE_BUILD_TYPE=Debug
cmake --build build/zco -j 2
(cd build/zco && ctest --output-on-failure)
cmake --install build/zco --prefix /path/to/prefix
```

仅构建库可加 `-DBUILD_TESTING=OFF`，共享库可加 `-DBUILD_SHARED_LIBS=ON`。
整体构建用法见[根 README](../README.md)，整体构建后用 `ctest -R '^zco_'`
筛选任务调度、栈、等待、同步、描述符和关闭流程测试。

消费者通过 `-DCMAKE_PREFIX_PATH=/path/to/prefix` 查找安装包：

```cmake
find_package(zco 1 CONFIG REQUIRED)
add_executable(app main.cc)
target_link_libraries(app PRIVATE zco::zco)
```

## 性能与源码

```bash
cmake --preset relwithdebinfo -DBUILD_BENCHMARKS=ON
cmake --build --preset relwithdebinfo --target zco_performance -j 2
```

以上命令从仓库根目录执行。开启性能程序时才启用内部栈复制统计，性能驱动位于
[tests/benchmark](tests/benchmark)。覆盖率入口见[根 README](../README.md)。

公共入口为 [zco.h](include/zco/zco.h)。实现按 [runtime](src/runtime)、
[execution](src/execution)、[wait](src/wait)、[sync](src/sync) 和 [io](src/io)
划分，分别负责调度、上下文与栈、等待、同步和 Linux I/O。
