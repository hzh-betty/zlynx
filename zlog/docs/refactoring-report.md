# zlog 重构实施记录：接口边界与所有权

实施日期：2026-10-09。依据同日的 `architecture-review.md`，本轮完成实施顺序中的阶段 1、2；没有完成整份迁移计划。

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
