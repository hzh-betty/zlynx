cmake_minimum_required(VERSION 3.18)

project(zco VERSION 3.0.0 LANGUAGES CXX)

# zco 可作为根工程子模块，也可作为独立包构建。
# 独立构建时从 ../cmake 加载公共 helper；核心只依赖 Threads。
if(NOT COMMAND zlynx_apply_common_options)
    set(ZLYNX_CMAKE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
    list(APPEND CMAKE_MODULE_PATH "${ZLYNX_CMAKE_DIR}")
    include(ZlynxOptions)
    include(ZlynxPackage)
    include(ZlynxTesting)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "zco currently supports Linux only.")
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    include(CTest)
endif()

find_package(Threads REQUIRED)

set(ZCO_SOURCES
    src/runtime/runtime.cc
    src/runtime/coroutine.cc
    src/runtime/worker.cc
    src/runtime/task_queues.cc
    src/execution/continuation.cc
    src/execution/stack_arena.cc
    src/execution/linux/native_context.cc
    src/wait/wait_state.cc
    src/wait/wait_queue.cc
    src/wait/timer_queue.cc
    src/io/descriptor.cc
    src/io/operations.cc
    src/io/linux/epoll_reactor.cc
    src/sync/event.cc
    src/sync/mutex.cc
    src/sync/wait_group.cc
)

add_library(zco ${ZCO_SOURCES})
add_library(zco::zco ALIAS zco)

# zco 公共头文件会被 znet/zhttp 和外部消费者使用，因此 include 是 PUBLIC。
target_include_directories(zco
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(zco
    PUBLIC
        Threads::Threads
)
zlynx_apply_common_options(zco)

# EXPORT_NAME 保证安装后目标名为 zco::zco。
set_target_properties(zco PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    EXPORT_NAME zco
)

# 测试专用 zco_stdalloc 使用同一份源码，但不链接项目级 zmalloc_override。
# 这样 allocator 敏感的协程/栈测试不会被全局分配器替换影响。
if(BUILD_TESTING OR ZLYNX_BUILD_PERF_TESTS)
    add_library(zco_stdalloc STATIC ${ZCO_SOURCES})
    target_include_directories(zco_stdalloc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(zco_stdalloc PUBLIC Threads::Threads)
    zlynx_apply_common_options(zco_stdalloc)
    if(ZLYNX_BUILD_PERF_TESTS)
        target_compile_definitions(zco_stdalloc PRIVATE ZCO_BENCHMARK_METRICS)
    endif()
endif()

# perf 源码在 tests 下，因此 BUILD_TESTING 或 perf 打开时需要进入 tests 子目录。
if(BUILD_TESTING OR ZLYNX_BUILD_PERF_TESTS)
    add_subdirectory(tests)
endif()

zlynx_install_module(zco zco
    VERSION ${PROJECT_VERSION}
    DEPENDENCIES Threads
)
