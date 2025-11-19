cmake_minimum_required(VERSION 3.18)
project(zlog LANGUAGES CXX)

# Use at least C++11
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find dependencies
find_package(fmt REQUIRED CONFIG)

# Output directories for libraries (place them in build/lib)
#set(LIB_OUTPUT_DIR ${CMAKE_SOURCE_DIR}/lib)
#set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${LIB_OUTPUT_DIR})
#set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${LIB_OUTPUT_DIR})

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

# Link dependencies
target_link_libraries(zlog_shared PUBLIC fmt::fmt)
target_link_libraries(zlog_static PUBLIC fmt::fmt)

# Bench executable
add_executable(bench bench/bench.cc)
target_include_directories(bench PRIVATE ${PROJECT_SOURCE_DIR}/include)

target_link_libraries(bench PRIVATE zlog_shared)

# Link pthread on POSIX systems if required by user code
if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(zlog_shared PUBLIC Threads::Threads)
    target_link_libraries(zlog_static PUBLIC Threads::Threads)
    target_link_libraries(bench PRIVATE Threads::Threads)
endif()

# Put bench executable into a bin directory inside the build tree
set_target_properties(bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

message(STATUS "zlog: shared library -> ${LIB_OUTPUT_DIR}")
message(STATUS "zlog: static  library -> ${LIB_OUTPUT_DIR}")
message(STATUS "bench executable -> ${CMAKE_BINARY_DIR}/bin")
