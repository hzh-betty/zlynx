cmake_minimum_required(VERSION 3.18)

project(zlynx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(zlog)

set(SOURCES
        coroutime/fiber.cc
        coroutime/scheduler.cc
        sync/sem.cc
        sync/thread.cc
)
set(HEADERS
        coroutime
        zlynx_util
        sync
)
add_executable(${PROJECT_NAME} main.cc ${SOURCES})
target_include_directories(${PROJECT_NAME} PRIVATE ${HEADERS})
target_link_libraries(${PROJECT_NAME} PRIVATE zlog_shared)