# CMake 构建系统审计

## 范围与目标

审计覆盖根工程、五个模块（`zmalloc`、`zlog`、`zco`、`znet`、`zhttp`）、测试、依赖、Preset、安装导出、覆盖率脚本和构建文档。目标是让每个模块目录可复制到仓库外独立构建，同时保留根工程聚合构建；各项目版本统一为 1.0.0，构建入口以简单的 CMake 原生 target 为主。

## 原结构

根 CMakeLists 负责全局语言标准、模块路径、编译选项和 allocator override 注入，并加载 `cmake/` 中的 options、package、testing 三个通用模块。模块可独立构建，但需回到仓库父目录加载这些 helper。根工程版本为 2.0.0，各模块版本分别为 3.0.0 或 2.0.0。

Preset 使用 schema 6、固定 Clang/Ninja 和共享库，包含 Debug、Release、Coverage、Perf 等配置。测试封装按 unit/integration/perf 划分自定义 target，并以统一命名规则组织。`zhttp` 同时暴露整体库和 `zhttp::core`；根工程可自动把 allocator override 加到多个运行时模块。

## 发现的问题

| 范围 | 发现 | 影响 |
|---|---|---|
| 独立模块 | 依赖仓库根目录的 `../cmake` helper | 模块目录无法作为自包含源码包使用 |
| 全局编译状态 | 根目录设置 C++ 标准、扩展模式和编译数据库；helper 统一施加编译选项 | 子项目配置被隐式影响，单独使用时行为与聚合构建不同 |
| 运行时依赖 | 根目录隐式向多个 target 注入 allocator override | 链接策略难从模块本身判断，安装消费不够显式 |
| 模块版本 | 根与各模块版本不一致 | package version、SONAME 和文档无法统一表达 |
| Target 边界 | `zhttp` 的 core/整体 target 有重复源码与额外链接关系 | 测试、安装和消费者需要理解不必要的内部 target |
| 测试封装 | 自定义测试 helper、分类 target 和额外命名约束 | CTest 已有能力重复实现，入口增加理解成本 |
| 配置矩阵 | Preset 固定编译器、共享库并带有额外覆盖率/性能配置 | 个人环境难以复用，基础配置不够精简 |
| 依赖声明 | 部分模块通过仓库路径或系统默认头文件隐式获得依赖 | 导入目标和消费端依赖不够明确 |
| 覆盖率脚本 | 仅首次配置时传递选项；缓存存在时可能复用未插桩构建；使用固定编译器和较新 CTest 参数 | 结果可能依赖旧缓存，且无法在 CMake 3.18 执行 |
| 文档 | 构建说明依赖旧 preset、target、版本和 CTest 参数 | 按文档操作会得到与目标架构不一致的配置 |

## 目标设计

每个模块拥有独立的 `CMakeLists.txt` 和自己的 package config 模板，使用 CMake imported targets 声明依赖，并直接管理库、测试与可选基准 target。根工程只设置项目版本、启用 CTest 并按依赖顺序添加子目录。模块测试在独立构建时自行启用 CTest。

保留一套最小构建入口：Debug、Release、RelWithDebInfo。每个模块安装并导出自身公开库 target；allocator override 由最终程序显式链接。普通测试直接使用 CTest，不添加分类 target 或测试框架。

## 版本决策

工程命令的最低版本为 CMake 3.18。Preset 文件使用 schema 2，因为 build/test presets 从 CMake 3.20 才可用；因此直接使用 presets 的最低版本是 3.20。低版本用户可使用 `cmake -S/-B`、`cmake --build` 和从构建目录执行 `ctest`。该分层保留 3.18 核心构建支持，同时避免伪称 schema 2 可由 3.18 解析。
