# zlog 重构实施记录：接口边界与所有权

实施日期：2026-10-09。依据同日的 `architecture-review.md`，本轮完成实施顺序中的阶段 1、2；没有完成整份迁移计划。

本文按时间保留各阶段记录；当前接口和验证以文末 2026-10-10 补充为准。

## 已实施

- `logger.h` 只保留 Logger、SyncLogger、AsyncLogger 和公共异步策略枚举。Builder 与注册表分别迁入 `logger_builder.h/.cc`、`logger_registry.h/.cc`，`zlog.h` 继续提供聚合入口。
- 异步装配迁入 `async_logger.cc`；Buffer、AsyncLooper、Spinlock 迁入 `src/async` 与 `zlog::detail`，不再安装内部队列、缓冲或锁的声明。核心和 Builder 公共头不再传递注册表或内部实现头。
- 注册表直接创建缺省 SyncLogger，解除注册表对 Builder 的依赖。局部构建保持显式创建，不访问全局注册表。
- Logger 按值接收名称并拥有不可变的 `std::string`；Builder 使用自有名称副本，保留未设置名称、空名称、清除名称配置三种行为。LogMessage 格式化期间借用 Logger 自有字符串，不借用调用方存储。
- Buffer 禁止复制，继续通过 `swap()` 交换独占内存；AsyncLogger 用 `unique_ptr` 独占 worker，析构在实现文件定义，先排空并回收 worker，再释放基类持有的 sink。
- sink 列表改为常量引用输入；Builder 的状态收为私有，并直接删除复制操作。移走锁后没有调用者的 NonCopyable 一并删除，File 工具暂留到后续阶段处理。
- 更新 CMake、相关调用方、README 和内部测试的私有 include 路径。新增代码注释使用中文。

## 兼容性

重构初期的验证构建曾使用 **2.0.0** 和 `SOVERSION 2`。当前保留 CMake 中的 **1.0.0** 版本设置；对象布局、名称与 sink 参数声明、内部类型边界发生变化，消费者仍必须重新编译。下述早期安装与 ABI 验证结果对应当时的版本设置。

直接使用 Builder、LoggerType 或 LoggerManager 的代码应分别包含 `zlog/logger_builder.h` 或 `zlog/logger_registry.h`。`zlog/zlog.h` 仍包含这些公共入口。Buffer、AsyncLooper、Spinlock 不再是安装接口，NonCopyable 已删除。

`build_global()` 的重名行为本轮维持既有契约：返回新建实例，注册表保留原实例；报告提出的注册结果调整留待后续阶段明确迁移。ModuleLogger 已在后续清理中删除，无前缀宏仍待处理。

## 验证：阶段 1、2

环境：Clang 21.1.8、C++17、Ninja、Debug、共享库，关闭 zmalloc override。验证没有对无关模块实施代码修改。

| 项目 | 结果 |
|---|---|
| 修改前 zlog 基线 | 9/9 CTest 目标通过 |
| 修复前新增回归 | 名称被调用方改写、Builder 借用名称、Buffer 可浅拷贝三项均复现失败 |
| 修改后 zlog | 10/10 CTest 目标通过，新增 8 个所有权用例 |
| ASan 所有权回归 | 8/8 用例通过；覆盖动态名称销毁、ModuleLogger 销毁后保留实例、worker 排空与 sink 析构顺序 |
| 公共头独立编译 | 9 个顶层公共头和暂留的 util 头均独立通过 `-Wall -Wextra -Werror` 编译 |
| 头文件依赖检查 | 核心与 Builder 头不传递注册表、internal 或 async 头 |
| 安装消费 | `find_package(zlog 2 CONFIG REQUIRED)` 编译、链接、运行通过；含独立核心头构造同步/异步日志器、局部构建和全局注册 |
| 安装边界与 ABI | 没有安装 Buffer、AsyncLooper、Spinlock 头；SONAME 为 `libzlog.so.2` |
| 性能程序编译 | zlog_performance 成功；缺少 spdlog/glog，第三方对比 benchmark 未构建 |
| 全仓构建 | 成功，包含 zhttp 的生产日志调用方 |
| 全仓 CTest | 75/75 测试目标通过，包含 zlog 与 zhttp 单元和集成测试 |

独立构建与测试可复现为：

```bash
cmake -S zlog -B build/validation/zlog -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON -DBUILD_SHARED_LIBS=ON \
  -DZLYNX_BUILD_PERF_TESTS=ON -DZLYNX_USE_ZMALLOC_OVERRIDE=OFF
cmake --build build/validation/zlog -j 4
ctest --test-dir build/validation/zlog --output-on-failure -j 2
cmake --install build/validation/zlog --prefix "$PWD/build/validation/install"
```

ASan 回归只运行本轮相关用例，没有据此声称整库通过 ASan：

```bash
cmake -S zlog -B build/validation/asan -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON -DBUILD_SHARED_LIBS=ON \
  -DZLYNX_BUILD_PERF_TESTS=OFF -DZLYNX_USE_ZMALLOC_OVERRIDE=OFF \
  -DCMAKE_CXX_FLAGS='-fsanitize=address -fno-omit-frame-pointer' \
  -DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address \
  -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address
cmake --build build/validation/asan --target zlog_ownership_test -j 4
build/validation/asan/tests/zlog_ownership_test
```

全仓验证使用相同的 Debug、共享库和系统 allocator 策略：

```bash
cmake -S . -B build/validation/repo -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON -DBUILD_SHARED_LIBS=ON \
  -DZLYNX_BUILD_PERF_TESTS=OFF -DZLYNX_USE_ZMALLOC_OVERRIDE=OFF
cmake --build build/validation/repo -j 4
ctest --test-dir build/validation/repo --output-on-failure -j 4
```

## 剩余阶段与已知问题

- 阶段 3：正文和 sink 的长度传递已在下述补充工作中修复；pattern 尾部、时间缓存键和文件错误检查仍待处理。
- 阶段 4：公共 `flush()/close()`、后台错误传播和共享 sink 同步仍待处理。后续清理删除了注册表替换入口，移除了锁内释放旧实例和并发替换 root 的路径。
- 异步容量检查还需要统一最大值、溢出和扩容失败行为；本轮只修改 Buffer 的拥有关系，没有将容量边界列为已修复。
- 阶段 5：ModuleLogger 已删除；收回 File 工具、调整无前缀宏与注册结果、完成最终目录和 API 迁移仍待处理。性能比较仍需先统一输出策略与完成语义。

本轮没有运行 TSan、全库 ASan 或性能对比，不据此推断并发安全、全部边界正确性或性能收益。

## 补充：C++11 兼容与字符串视图

根据后续要求，将源码最低标准调整为 C++11，允许使用更高标准编译。移除
`std::optional`、`std::make_unique`、C++17 嵌套命名空间定义及测试中的类型特征
变量模板。Builder 改用自有字符串与名称是否已设置的标志，保留空名称和
`nullptr` 清除配置的行为；worker 仍由 `unique_ptr` 独占。内部 AsyncLooper
提供匹配的对齐分配与释放，保留原有缓存行对齐而不依赖 C++17 的对齐 new。

公共构建 helper 支持按模块声明最低标准，zlog 导出 `cxx_std_11`；其他模块
沿用 C++17。调用方设置更高标准时按其选择编译。

日志格式串、LogMessage 正文和名称采用 `fmt::string_view`。正文通过缓冲区的
已知长度传入，移除额外的零终止字符与格式化时的重复长度扫描；内置 sink
使用指定长度的视图输出。Builder 的视图入口复制名称，Logger 和 Formatter
保留长期状态的所有权。已有 LogMessage C 字符串入口继续将空指针视为空内容。

该阶段新增固定使用严格 C++11 的独立测试，以及无零终止格式串/名称/正文、内嵌 NUL、
空视图、同步/异步完整输出和文件滚动的精确字节回归。

补充验证结果（下述接口清理之前的快照）：

| 项目 | 结果 |
|---|---|
| 严格 C++11 检查 | 10 个公共头独立编译、全部 13 个生产实现文件通过 `-std=c++11 -pedantic-errors -Wall -Wextra -Werror` |
| C++11 生产库 | 13 个实现文件实际使用 `-std=c++11` 构建，12/12 CTest 目标通过；zlog_performance 编译成功 |
| C++17 全仓 | zlog 的 13 个实现文件实际使用 `-std=c++17` 构建，77/77 CTest 目标通过 |
| 独立 C++11 入口 | 两种构建下均固定使用 C++11，源码通过 `__cplusplus` 断言防止被升级为高标准 |
| 安装消费 | 导出特性确认为 `cxx_std_11`；全部安装头独立通过 C++11 编译，`find_package(zlog 2)` 的 C++11 消费程序编译、链接、运行成功 |
| ASan/UBSan | C++11 库配合独立兼容、所有权和字符串视图三个测试目标，3/3 通过；开启泄漏检测和 UBSan 遇错终止 |

本机 GTest/GMock 1.17.0 自身要求 C++17，因此框架测试按其要求编译；该依赖
不提高 zlog 库和安装接口的标准。LogMessage 的公共布局本轮继续调整，消费者
应按上述接口迁移要求重新编译。没有据此声称性能吞吐提升，也未运行
整个仓库的 sanitizer 测试。

本次 C++11 验证命令：

```bash
cmake -S zlog -B build/validation/cxx11 -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_STANDARD=11 \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DBUILD_SHARED_LIBS=ON \
  -DZLYNX_BUILD_PERF_TESTS=ON -DZLYNX_USE_ZMALLOC_OVERRIDE=OFF
cmake --build build/validation/cxx11 -j 4
ctest --test-dir build/validation/cxx11 --output-on-failure -j 2
```

## 补充：删除兼容测试与模块日志封装

接口清理阶段按要求直接在 `main` 工作区修改，当时未创建提交。删除 `tests/compat/cxx11_test.cc`
及其 CMake 目标；源码和安装接口的最低标准仍为 C++11，兼容性通过实际编译验证。

删除 `module_logger.h/.cc`、对应单元测试及所有权测试中依赖该封装的用例；删除
`LoggerManager::upsert_logger()` 的声明、实现和三个替换行为测试。注册表保留
`add_logger()`、`get_logger()` 和 `root_logger()`，同名添加继续保留已有实例。
仓库内没有这些已删除接口的生产调用方，README 已补充迁移说明。

此前验证表保留各阶段的历史结果；当前清理后的验证如下，均使用 Clang 21.1.8、
Debug、共享库，并关闭 zmalloc override：

| 项目 | 结果 |
|---|---|
| 严格 C++11 检查 | 9 个公共头、全部 12 个生产实现文件通过 `-std=c++11 -pedantic-errors -Wall -Wextra -Werror` |
| C++11 独立构建与测试 | 12 个生产实现文件实际使用 C++11 编译，10/10 CTest 目标通过；构建目录为 `build/zlog-api-cleanup-cxx11` |
| C++17 全仓构建与测试 | 含 zhttp 日志调用方的全仓构建成功，75/75 CTest 目标通过；构建目录为 `build/zlog-api-cleanup-repo` |
| 安装接口 | 新安装目录包含 9 个公共头并导出 `cxx_std_11`；没有 ModuleLogger 头文件或 ModuleLogger/upsert_logger 动态导出符号 |

## 补充：分批提交验证

后续按既有提交格式拆为构建要求、接口与所有权、字符串视图与长度修复、文档四批。
每个代码批次使用暂存快照独立构建，未将其他模块的工作区修改混入提交。当前版本
设置保留为 1.0.0；下表为该设置下的 C++11、Debug、共享库验证结果：

| 批次 | 结果 |
|---|---|
| C++11 构建要求 | 暂存快照构建成功，9/9 CTest 目标通过 |
| 接口与所有权 | 暂存快照构建成功，9/9 CTest 目标通过，包含 7 个所有权回归用例 |
| 字符串视图与长度 | 暂存快照构建成功，10/10 CTest 目标通过；9 个公共头严格 C++11 独立编译通过，12 个生产实现文件实际使用 C++11 |
| 安装接口 | 新安装包导出 `cxx_std_11`，SONAME 为 `libzlog.so.1`；没有 ModuleLogger 头文件或 ModuleLogger/upsert_logger 动态导出符号 |

## 补充：2026-10-10 正确性与完成语义修复

对照仓库 `tmp/spdlog-1.x` 并复现问题后，按要求删除 `build_global()` 和
`tests/unit/string_view_test.cc`。使用 `build()` 构建，再显式添加到注册表；
空指针与重名注册均报告 `std::invalid_argument`，不会返回未注册的新实例。

本轮修复：

- 正文和序列化缓冲区继续使用 `thread_local` 复用容量，最外层调用独占借用
  缓存；缓冲区正在使用时，嵌套调用使用独立的临时缓冲区。占用标记在正常和
  异常退出时均归还，跨 logger 嵌套日志不会覆盖外层数据或造成扩容后的悬空
  访问。LogMessage 保持局部对象，正文仍借用本次调用的缓冲区；它本身不动态
  分配内存。同一 logger 的 sink 回调重入明确报告错误，避免自锁或后台线程
  等待自身。
- 内置 sink 自身加锁，允许多个同步/异步 logger 共享；检查目录、打开、
  写入与刷新错误。多 sink 输出尝试所有目标后再报告首个错误。
- 公开 `flush()/close()`。异步刷新等待请求前已接收记录输出并刷新 sink；
  关闭拒绝新日志、排空队列、刷新并 join，然后释放 sink 所有权。关闭可重复，
  错误在清理后报告，共享 sink 不会被一个 logger 提前关闭。析构不抛出，
  未显式关闭时的错误输出到 stderr。
- 异步双缓冲保留长度头，批量收取后逐条回调；一条记录或一个 sink 失败
  不会跳过后续记录或其他 sink，后台首个异常由 `flush()/close()` 重抛。
- 格式解析保存尾部文本和字面 `%`；时间缓存继续按线程、秒复用，同时
  校验格式串。跨 Formatter 和同一 pattern 的多个时间格式不会混用文本。
- Buffer 用减法检查最大容量，扩容不超限，空间不足或分配失败明确报错；
  入队前预留完整记录，避免半条记录。长度头计入 safe 2 MiB / unsafe
  512 MiB 容量，单条超限立即拒绝，等待时间必须为正数。
- 滚动 sink 按单条记录检查大小，单条超限拒绝并报告错误；独占创建文件
  避免同秒重建时复用旧文件。默认保留 10 个文件（含当前文件），第四个
  参数 `max_files` 可配置，清理范围含前次运行留下的同 basename 文件。
  同一 basename 由一个共享 sink 管理，多个独立 sink 的保留策略不协调。
- 异步测试使用确定的刷新/关闭屏障，并核对完整条数、内容和记录边界。
  性能程序统一逐条刷新设置，排空与刷新纳入计时，吞吐倒数不再标作平均延迟。

保留仍有效的中文注释，旧实现已经不适用的说明随逻辑更新。新增虚函数与成员
改变 ABI，因此 zlog 更新为 2.0.0、`SOVERSION 2`，所有调用方须重新编译。
`flush()` 表示输出并刷新用户态缓冲，不提供 `fsync` 的断电持久性保证。

验证环境：Clang 21.1.8、Ninja、独立库 C++11、Debug；全仓 C++17、共享库，
使用默认开启的 zmalloc override。sanitizer 独立构建使用系统 allocator。

| 项目 | 结果 |
|---|---|
| 独立 zlog 构建和 CTest | 9/9 目标通过（已删除 string_view_test 目标） |
| ASan/UBSan | zlog 全部 9/9 目标通过，开启泄漏检测与遇错终止 |
| TSan | zlog 全部 9/9 目标通过，覆盖共享 sink、关闭竞争、刷新/停止和背压 |
| 严格 C++11 | 9 个安装头、12 个生产实现通过 `-std=c++11 -pedantic-errors -Wall -Wextra -Werror` |
| 全仓编译与 CTest | 全仓含性能程序编译成功，77/77 目标通过，包含 zhttp 调用方 |
| 安装消费与 ABI | `find_package(zlog 2 CONFIG REQUIRED)` 的严格 C++11 程序编译、链接、运行通过；SONAME 为 `libzlog.so.2`；新增 flush/close 符号存在，build_global 符号已删除 |
| 性能程序冒烟 | 4 线程、1003 条、128 字节，同步/异步实际文件均为完整 1003 行，无固定刷新等待 |

后续按性能要求恢复正文和序列化的 `thread_local` 缓存，增加异常安全的占用标记。
普通调用只清空内容并复用容量；嵌套调用使用临时缓冲区，保留最外层正在使用的
数据。新增回归覆盖参数格式化已经写入部分正文时的重入、格式化异常后继续写入，
以及三层 sink 嵌套输出。恢复缓存后重新通过 zlog 的普通、ASan/UBSan、TSan
各 9/9 目标、全仓 77/77 目标，以及严格 C++11 编译和安装包消费检查。

分配探针使用无自身分配的计数 sink、`%m` pattern、4096 字节正文；预热一次后
写入 20000 条，计数全局 C++ `new/new[]` 调用。构造 logger、生成正文和抛出
测试异常均在计数区间之外，测试程序位于 `/tmp/zlog-fix-KoH6Pj/buffer_reuse_probe.cc`。

| 测量条件 | 每次调用使用局部缓冲区 | 恢复缓存后 |
|---|---:|---:|
| 预热后的 20000 条普通日志 | 40000 次分配 | 0 次分配 |
| sink 抛出异常后的 20000 条普通日志 | 40000 次分配 | 0 次分配 |
| 格式化抛出异常后的 20000 条普通日志 | 40000 次分配 | 0 次分配 |

该探针验证缓存复用和异常退出后归还，不代表任意参数、sink 或异步队列均无分配，
也不据此推断实际磁盘吞吐提升。

构建与验证产物位于 `/tmp/zlog-fix-KoH6Pj`，未写入仓库构建目录。
第三方对比 benchmark 的完成计时已更新，但本机缺少 spdlog/glog 安装依赖，
该目标未编译运行；不据冒烟数据推断性能提升，也未对其他模块运行 sanitizer。

本轮按既有提交格式分为格式与容量、日志器与输出、性能测试、文档四批。
代码批次从暂存区导出独立快照验证，避免后续批次的修改掩盖当前提交的问题。
性能脚本原有 CRLF 换行导致 Bash 解析失败，本轮改为 LF，并更新统计字段和
中文说明；脚本语法检查与 `--help` 入口均通过。

| 提交批次 | 暂存快照验证结果 |
|---|---|
| 格式与缓冲容量 | C++11、Debug、共享库构建成功，10/10 CTest 目标通过 |
| 日志器接口、重入、异步输出与文件轮转 | C++11、Debug、共享库构建成功，9/9 CTest 目标通过 |
| 性能测试 | 性能程序编译运行成功，4 线程、1003 条、128 字节的同步与异步文件各为完整 1003 行；Bash 语法与帮助入口通过 |

各批次快照与验证产物位于 `/tmp/zlog-commits-3mZLcL`。文档批次同步公开接口、
迁移要求、缓存复用和验收结果，历史评审记录保留并注明当前状态。
