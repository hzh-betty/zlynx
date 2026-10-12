# CMake 重构结果

## 最终结构

根工程只声明 `zlynx 1.0.0`、启用 CTest，并依次添加五个模块。每个模块目录包含独立的 `CMakeLists.txt` 和 `<module>Config.cmake.in`，可复制后单独配置、构建、测试和安装。

核心 target 与依赖关系：

```text
zmalloc::zmalloc
zmalloc::override ──> zmalloc::zmalloc
zlog::zlog ──> fmt::fmt, Threads::Threads
zco::zco ──> Threads::Threads
znet::znet ──> zco::zco (PUBLIC), OpenSSL (PRIVATE), Threads (PRIVATE)
zhttp::zhttp ──> znet::znet, zco::zco, nlohmann_json (PUBLIC)
                zlog::zlog, fmt, toml11, OpenSSL, zlib, Brotli, Threads (PRIVATE)
```

跨模块依赖通过安装后的 Config Package 查找；根工程先添加依赖模块，因此聚合构建直接复用已存在的 target。`zhttp` 现在只有一个产品库 target。每个库发布 `1.0.0`，共享库的 `SOVERSION` 为 1。`zlog`/`zmalloc` 保持 C++11，其余模块保持 C++17；编译标准由 target 声明。

## 关键改动

- 根目录移除全局编译设置、目录 include、公共 helper 和 allocator override 隐式注入。
- 五个模块用明确源文件列表声明库、依赖、警告、安装导出和 package config；模块可脱离仓库独立构建。
- 删除根 `cmake/` 中的通用 options/package/testing helper。移除 `zhttp::core`，合并为 `zhttp::zhttp`。
- CTest 测试直接对应独立测试 executable；测试只链接所需生产库。保留有必要的共享库测试包装行为。
- Preset 精简为 Debug、Release、RelWithDebInfo；不固定编译器或 BUILD_SHARED_LIBS。
- 文档和性能脚本更新为新 target、preset、版本和独立模块工作流。覆盖率脚本每次显式配置插桩参数，并兼容 CMake 3.18 的 CTest 用法。

## 破坏性变更

- 所有包版本统一为 1.0.0，旧的模块版本请求需调整；共享库 SONAME 主版本为 1，使用旧 ABI 主版本的消费者需重新链接。
- 移除旧的全局选项、`ZLYNX_*` 构建开关、unit/integration/perf 分类测试 target 和旧 Preset 名称。基准通过 `BUILD_BENCHMARKS` 选项构建。
- 移除 `zhttp::core`，消费者统一链接 `zhttp::zhttp`。
- allocator override 不再由顶层自动注入，应用需显式链接 `zmalloc::override`。
- 跨模块独立构建需先安装依赖模块，并通过 `CMAKE_PREFIX_PATH` 提供安装前缀。
- 核心 CMake 最低版本为 3.18；Preset schema 2 的 build/test preset 需 CMake 3.20+。3.18 用户使用普通 `-S/-B` 命令。

## 验证

验证记录来自本次重构的隔离构建目录，未复用旧项目构建缓存。

| 验证 | 结果 |
|---|---|
| GCC Debug 静态构建与 CTest | 82/82 通过 |
| GCC Release 静态构建与 CTest | 82/82 通过 |
| GCC RelWithDebInfo、基准目标与 CTest | 82/82 通过 |
| GCC/Clang Debug 共享库构建与 CTest | 各 83/83 通过 |
| Clang UBSan 静态构建与 CTest | 82/82 通过 |
| zlog ASan 测试 | 9/9 通过 |
| 五个复制到仓库外的模块独立构建、测试和安装 | 全部通过；测试分别 16、9、14、8、35 项 |
| 静态和共享安装前缀迁移、下游 CMake 消费 | 各 6/6 consumer 通过 |
| Ninja Multi-Config Debug/Release 代表测试 | 每个配置 3/3 通过 |
| CMake 3.18.4 全模块 Debug/shared 构建、CTest、独立模块与安装消费 | 83/83；五个模块和 6 个 consumer 均通过 |
| CMake 3.20.5 schema 2 preset 配置、代表构建与测试 | 三个 preset 可配置；4/4 代表测试通过 |
| Shell 脚本语法、差异空白和 target/source 检查 | 通过 |

## 剩余限制

- 完整 ASan 下，allocator override 的两个测试与 ASan 对 malloc/free 的拦截发生真实冲突；未禁用测试或更改 allocator 业务行为。其余相关静态、共享与 UBSan 测试结果如上。
- 本次没有验证非 Linux 平台、aarch64、不同 C++ 标准库或 Windows/MSVC；`zco`、`znet`、`zhttp` 按现有实现明确要求 Linux。
- `zlog_benchmark` 依赖 spdlog；未安装 spdlog 时该可选对比程序不生成，并由配置输出说明。其他基准目标仍可构建。
- 未运行完整五模块 HTML 覆盖率报告、wrk 压测或 perf/valgrind 基准；这些性能结果没有作为构建成功条件。
