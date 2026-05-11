cmake_minimum_required(VERSION 3.18)

project(zlynx
    VERSION 1.0.0
    DESCRIPTION "Coroutine-based high-performance network component libraries"
    LANGUAGES CXX
)

# 生成器和默认编译器放在 CMakePresets.json 中声明：
# - preset 默认使用 Ninja + clang++；
# - CMakeLists.txt 不强行改 CMAKE_GENERATOR/CMAKE_CXX_COMPILER，方便外部工具链接管。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(ZLYNX_CMAKE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
list(APPEND CMAKE_MODULE_PATH "${ZLYNX_CMAKE_DIR}")

# 三个公共 helper 分别负责：
# - ZlynxOptions：target 级编译选项、coverage、perf 参数；
# - ZlynxPackage：install/export/find_package；
# - ZlynxTesting：unit/integration/stress/perf 测试目标封装。
include(ZlynxOptions)
include(ZlynxPackage)
include(ZlynxTesting)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

include(CTest)
find_package(Threads REQUIRED)

# 按依赖顺序添加模块，保证模块内部只需要 target_link_libraries，
# 不再由子模块自行 add_subdirectory 兄弟目录。
add_subdirectory(zmalloc)
add_subdirectory(zlog)
add_subdirectory(zco)
add_subdirectory(znet)
add_subdirectory(zhttp)

# zmalloc_override 是项目级运行时策略：
# - ON：源码树整体构建时把 override 私有链接到运行时模块；
# - OFF：所有模块使用系统 allocator；
# - install/export 不把该策略传播为各模块 PUBLIC 依赖。
if(ZLYNX_USE_ZMALLOC_OVERRIDE AND TARGET zmalloc_override)
    foreach(runtime_target zlog zco znet zhttp)
        if(TARGET ${runtime_target})
            target_link_libraries(${runtime_target} PRIVATE zmalloc_override)
        endif()
    endforeach()
endif()

message(STATUS "")
message(STATUS "================ zlynx configuration ================")
message(STATUS "Version             : ${PROJECT_VERSION}")
message(STATUS "Generator           : ${CMAKE_GENERATOR}")
message(STATUS "C++ compiler        : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "Build type          : ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ standard        : 14")
message(STATUS "Build shared libs   : ${BUILD_SHARED_LIBS}")
message(STATUS "Build testing       : ${BUILD_TESTING}")
message(STATUS "Coverage            : ${ZLYNX_ENABLE_COVERAGE}")
message(STATUS "zmalloc override    : ${ZLYNX_USE_ZMALLOC_OVERRIDE}")
message(STATUS "Build stress bins   : ${ZLYNX_BUILD_STRESS_TESTS}")
message(STATUS "Build perf bins     : ${ZLYNX_BUILD_PERF_TESTS}")
message(STATUS "Install prefix      : ${CMAKE_INSTALL_PREFIX}")
message(STATUS "Modules             : zmalloc, zlog, zco, znet, zhttp")
message(STATUS "======================================================")
