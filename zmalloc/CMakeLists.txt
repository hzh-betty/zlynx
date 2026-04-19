cmake_minimum_required(VERSION 3.18)

project(zmalloc VERSION 1.0.0 LANGUAGES CXX)

# C++11 标准
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找线程库
find_package(Threads REQUIRED)

function(zmalloc_enable_coverage target_name)
    if(ENABLE_COVERAGE)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(${target_name} PRIVATE -O0 -g --coverage)
            target_link_options(${target_name} PUBLIC --coverage)
        else()
            message(WARNING "ENABLE_COVERAGE is ON, but compiler ${CMAKE_CXX_COMPILER_ID} is not supported")
        endif()
    endif()
endfunction()

# 源文件
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

# 头文件目录
set(ZMALLOC_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

# 构建库
add_library(zmalloc ${ZMALLOC_SOURCES})

add_library(zmalloc_override STATIC
    src/override.cc
)
zmalloc_enable_coverage(zmalloc)
zmalloc_enable_coverage(zmalloc_override)

target_include_directories(zmalloc
    PUBLIC
        $<BUILD_INTERFACE:${ZMALLOC_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include>
)

target_include_directories(zmalloc_override
    PUBLIC
        $<BUILD_INTERFACE:${ZMALLOC_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(zmalloc
    PUBLIC
        Threads::Threads
)

target_link_libraries(zmalloc_override
    PUBLIC
        zmalloc
)

# 编译选项
target_compile_options(zmalloc PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_compile_options(zmalloc_override PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

# 测试子目录
if(ENABLE_TESTS)
    add_subdirectory(tests)
endif()

# 导出
set_target_properties(zmalloc PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
)

set_target_properties(zmalloc_override PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    POSITION_INDEPENDENT_CODE ON
)
