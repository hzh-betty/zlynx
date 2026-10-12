cmake_minimum_required(VERSION 3.18)
project(zlog VERSION 1.0.0 LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    include(CTest)
endif()
option(BUILD_BENCHMARKS "Build benchmark executables" OFF)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

find_package(fmt CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_library(zlog
    src/async/buffer.cc
    src/async/looper.cc
    src/async_logger.cc
    src/format.cc
    src/level.cc
    src/logger.cc
    src/logger_builder.cc
    src/logger_registry.cc
    src/message.cc
    src/sink.cc
    src/util.cc
)
add_library(zlog::zlog ALIAS zlog)

set(zlog_cxx_standard 11)
target_compile_features(zlog PUBLIC cxx_std_${zlog_cxx_standard})
if(NOT CMAKE_CXX_STANDARD)
    set_property(TARGET zlog PROPERTY CXX_STANDARD ${zlog_cxx_standard})
endif()
set_target_properties(zlog PROPERTIES
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})
target_compile_options(zlog PRIVATE
    "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>"
    "$<$<CXX_COMPILER_ID:MSVC>:/W4;/permissive->")
target_include_directories(zlog
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(zlog PUBLIC fmt::fmt Threads::Threads)

if(BUILD_TESTING OR BUILD_BENCHMARKS)
    add_subdirectory(tests)
endif()

install(TARGETS zlog EXPORT zlogTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT zlogTargets NAMESPACE zlog::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zlog)
configure_package_config_file(cmake/zlogConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/zlogConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zlog)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/zlogConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/zlogConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/zlogConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zlog)
