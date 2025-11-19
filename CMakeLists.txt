cmake_minimum_required(VERSION 3.18)

project(zlynx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(zlog)

add_executable(${PROJECT_NAME} main.cc)
target_link_libraries(${PROJECT_NAME} PRIVATE zlog_shared)