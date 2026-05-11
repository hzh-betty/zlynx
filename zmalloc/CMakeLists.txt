cmake_minimum_required(VERSION 3.18)

project(zmalloc VERSION 1.0.0 LANGUAGES CXX)

# 模块既可被根工程 add_subdirectory，也可独立配置构建。
# 独立构建时需要手动加载公共 helper，保持 target/install/test 行为一致。
if(NOT COMMAND zlynx_apply_common_options)
    set(ZLYNX_CMAKE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
    list(APPEND CMAKE_MODULE_PATH "${ZLYNX_CMAKE_DIR}")
    include(ZlynxOptions)
    include(ZlynxPackage)
    include(ZlynxTesting)
endif()

find_package(Threads REQUIRED)

# allocator 主库只包含正常分配器实现；全局替换符号放在 zmalloc_override。
set(ZMALLOC_SOURCES
    src/size_class.cc
    src/system_alloc.cc
    src/free_list.cc
    src/transfer_cache.cc
    src/thread_cache.cc
    src/central_cache.cc
    src/page_cache.cc
    src/span_list.cc
)

add_library(zmalloc ${ZMALLOC_SOURCES})
add_library(zmalloc::zmalloc ALIAS zmalloc)

# 头文件作为 PUBLIC 用法需求导出，消费者链接 zmalloc::zmalloc 后自动获得 include。
target_include_directories(zmalloc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(zmalloc PUBLIC Threads::Threads)
zlynx_apply_common_options(zmalloc)

# 安装后保持版本化 soname，并导出为 zmalloc::zmalloc。
set_target_properties(zmalloc PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    EXPORT_NAME zmalloc
)

# zmalloc_override 提供全局 new/delete/malloc 替换符号。
# 它单独建成目标，避免所有消费者被迫启用 allocator override。
add_library(zmalloc_override STATIC src/override.cc)
add_library(zmalloc::override ALIAS zmalloc_override)
target_link_libraries(zmalloc_override PUBLIC zmalloc)
zlynx_apply_common_options(zmalloc_override)
set_target_properties(zmalloc_override PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    EXPORT_NAME override
)

# benchmark/perf 源码位于 tests/benchmark，所以 perf 构建也需要进入 tests。
if(BUILD_TESTING OR ZLYNX_BUILD_PERF_TESTS)
    add_subdirectory(tests)
endif()

zlynx_install_module(zmalloc zmalloc
    VERSION ${PROJECT_VERSION}
    DEPENDENCIES Threads
)
zlynx_install_extra_target(zmalloc zmalloc_override)
