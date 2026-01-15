cmake_minimum_required(VERSION 3.18)
project(zlog LANGUAGES CXX)

# Use at least C++11
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find dependencies
find_package(fmt REQUIRED CONFIG)

# Sources for zlog
set(ZLOG_SOURCES
    src/buffer.cc
    src/format.cc
    src/level.cc
    src/logger.cc
    src/looper.cc
    src/message.cc
    src/sink.cc
    src/util.cc
)

# Create both shared and static libraries
add_library(zlog_shared SHARED ${ZLOG_SOURCES})
add_library(zlog_static STATIC ${ZLOG_SOURCES})

# Public include dir
target_include_directories(zlog_shared PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_include_directories(zlog_static PUBLIC ${PROJECT_SOURCE_DIR}/include)
message(STATUS "Project source dir: ${PROJECT_SOURCE_DIR}")

# Link dependencies
target_link_libraries(zlog_shared PUBLIC fmt::fmt)
target_link_libraries(zlog_static PUBLIC fmt::fmt)

# Link pthread on POSIX systems if required by user code
if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(zlog_shared PUBLIC Threads::Threads)
    target_link_libraries(zlog_static PUBLIC Threads::Threads)
endif()

option(BUILD_TESTING "Build tests" OFF)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
