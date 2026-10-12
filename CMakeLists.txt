cmake_minimum_required(VERSION 3.18)
project(zlynx VERSION 1.0.0 LANGUAGES CXX)

include(CTest)

add_subdirectory(zmalloc)
add_subdirectory(zlog)
add_subdirectory(zco)
add_subdirectory(znet)
add_subdirectory(zhttp)
