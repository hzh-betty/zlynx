cmake_minimum_required(VERSION 3.18)

project(zlynx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 添加子项目
add_subdirectory(zlog)
add_subdirectory(zcoroutine)

# 原有代码（保留用于对比）
# set(SOURCES
#         coroutime/fiber.cc
#         coroutime/scheduler.cc
#         coroutime/timer.cc
#         coroutime/io_manager.cc
#         coroutime/fd_manager.cc
#         coroutime/hook.cc
#         sync/sem.cc
#         sync/thread.cc
# )

set(HEADERS
        zlynx_util
)

# 主程序（使用新的zcoroutine库）
add_executable(${PROJECT_NAME} main.cc)
target_include_directories(${PROJECT_NAME} PRIVATE ${HEADERS})
target_include_directories(${PROJECT_NAME} PRIVATE zcoroutine/include)
target_link_libraries(${PROJECT_NAME} PRIVATE zlog_shared zcoroutine_static pthread)