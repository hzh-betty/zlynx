cmake_minimum_required(VERSION 3.18)

project(zco VERSION 1.0.0 LANGUAGES CXX)

# zco 可作为根工程子模块，也可作为独立包构建。
# 独立构建时从 ../cmake 加载公共 helper，并通过 find_package(zlog) 获取依赖。
if(NOT COMMAND zlynx_apply_common_options)
    set(ZLYNX_CMAKE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
    list(APPEND CMAKE_MODULE_PATH "${ZLYNX_CMAKE_DIR}")
    include(ZlynxOptions)
    include(ZlynxPackage)
    include(ZlynxTesting)
endif()

if(NOT UNIX OR APPLE)
    message(FATAL_ERROR "zco currently supports Linux only.")
endif()

find_package(Threads REQUIRED)

# 在源码树内优先链接未命名空间的本地 target；独立构建/安装消费时使用 zlog::zlog。
if(TARGET zlog)
    set(ZCO_ZLOG_TARGET zlog)
else()
    find_package(zlog CONFIG REQUIRED)
    set(ZCO_ZLOG_TARGET zlog::zlog)
endif()

set(ZCO_SOURCES
    src/sched.cc
    src/zco_log.cc
    src/context.cc
    src/steal_queue.cc
    src/fiber_pool.cc
    src/shared_stack_buffer.cc
    src/snapshot_buffer_pool.cc
    src/fiber_handle_registry.cc
    src/runtime_manager.cc
    src/fiber.cc
    src/timer.cc
    src/epoller.cc
    src/processor.cc
    src/event.cc
    src/mutex.cc
    src/wait_group.cc
    src/pool.cc
    src/io_event.cc
    src/hook.cc
)

add_library(zco ${ZCO_SOURCES})
add_library(zco::zco ALIAS zco)

# zco 公共头文件会被 znet/zhttp 和外部消费者使用，因此 include 是 PUBLIC。
target_include_directories(zco
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(zco
    PUBLIC
        ${ZCO_ZLOG_TARGET}
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
    target_include_directories(zco_stdalloc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(zco_stdalloc PUBLIC ${ZCO_ZLOG_TARGET} Threads::Threads)
    zlynx_apply_common_options(zco_stdalloc)
endif()

# perf 源码在 tests 下，因此 BUILD_TESTING 或 perf 打开时需要进入 tests 子目录。
if(BUILD_TESTING OR ZLYNX_BUILD_PERF_TESTS)
    add_subdirectory(tests)
endif()

zlynx_install_module(zco zco
    VERSION ${PROJECT_VERSION}
    DEPENDENCIES zlog Threads
)
