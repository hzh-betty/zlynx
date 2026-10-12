cmake_minimum_required(VERSION 3.18)
project(zco VERSION 1.0.0 LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    include(CTest)
endif()
option(BUILD_BENCHMARKS "Build benchmark executables" OFF)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "zco currently supports Linux only")
endif()

find_package(Threads REQUIRED)

add_library(zco
    src/execution/continuation.cc
    src/execution/linux/native_context.cc
    src/execution/stack_arena.cc
    src/io/descriptor.cc
    src/io/linux/epoll_reactor.cc
    src/io/operations.cc
    src/io/wait_registry.cc
    src/runtime/coroutine.cc
    src/runtime/runtime.cc
    src/runtime/task_queues.cc
    src/runtime/worker.cc
    src/sync/event.cc
    src/sync/mutex.cc
    src/sync/wait_group.cc
    src/wait/timer_queue.cc
    src/wait/wait_queue.cc
    src/wait/wait_state.cc
)
add_library(zco::zco ALIAS zco)

set(zco_cxx_standard 17)
target_compile_features(zco PUBLIC cxx_std_${zco_cxx_standard})
if(NOT CMAKE_CXX_STANDARD)
    set_property(TARGET zco PROPERTY CXX_STANDARD ${zco_cxx_standard})
endif()
set_target_properties(zco PROPERTIES
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})
target_compile_options(zco PRIVATE
    "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>"
    "$<$<CXX_COMPILER_ID:MSVC>:/W4;/permissive->")
target_include_directories(zco
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(zco PUBLIC Threads::Threads)

if(BUILD_BENCHMARKS)
    target_compile_definitions(zco PRIVATE ZCO_BENCHMARK_METRICS)
endif()

if(BUILD_TESTING OR BUILD_BENCHMARKS)
    add_subdirectory(tests)
endif()

install(TARGETS zco EXPORT zcoTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT zcoTargets NAMESPACE zco::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zco)
configure_package_config_file(cmake/zcoConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/zcoConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zco)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/zcoConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/zcoConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/zcoConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zco)
