cmake_minimum_required(VERSION 3.18)
project(znet VERSION 1.0.0 LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    include(CTest)
endif()
option(BUILD_BENCHMARKS "Build benchmark executables" OFF)
option(ZNET_ENABLE_TLS "Enable OpenSSL-based TLS transport" ON)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "znet currently supports Linux only")
endif()

if(NOT TARGET zco::zco)
    find_package(zco 1 CONFIG REQUIRED)
endif()
find_package(Threads REQUIRED)

set(znet_transport_sources
    src/transport/socket.cc
    src/transport/tcp_stream.cc
)
if(ZNET_ENABLE_TLS)
    find_package(OpenSSL 1.1 REQUIRED COMPONENTS SSL Crypto)
    list(APPEND znet_transport_sources src/transport/tls_stream.cc)
else()
    list(APPEND znet_transport_sources src/transport/tls_disabled.cc)
endif()

add_library(znet
    src/byte_buffer.cc
    src/endpoint.cc
    src/error.cc
    src/server/acceptor.cc
    src/server/connection.cc
    src/server/session_manager.cc
    src/server/session_runner.cc
    src/server/tcp_server.cc
    ${znet_transport_sources}
)
add_library(znet::znet ALIAS znet)

set(znet_cxx_standard 17)
target_compile_features(znet PUBLIC cxx_std_${znet_cxx_standard})
if(NOT CMAKE_CXX_STANDARD)
    set_property(TARGET znet PROPERTY CXX_STANDARD ${znet_cxx_standard})
endif()
set_target_properties(znet PROPERTIES
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})
target_compile_options(znet PRIVATE
    "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>"
    "$<$<CXX_COMPILER_ID:MSVC>:/W4;/permissive->")
target_compile_definitions(znet PUBLIC ZNET_ENABLE_TLS=$<BOOL:${ZNET_ENABLE_TLS}>)
target_include_directories(znet
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(znet
    PUBLIC zco::zco
    PRIVATE Threads::Threads)
if(ZNET_ENABLE_TLS)
    target_link_libraries(znet PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

if(BUILD_TESTING OR BUILD_BENCHMARKS)
    add_subdirectory(tests)
endif()

install(TARGETS znet EXPORT znetTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT znetTargets NAMESPACE znet::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/znet)
if(ZNET_ENABLE_TLS)
    set(ZNET_OPENSSL_FIND_DEP "find_dependency(OpenSSL 1.1 COMPONENTS SSL Crypto)")
else()
    set(ZNET_OPENSSL_FIND_DEP "")
endif()
configure_package_config_file(cmake/znetConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/znetConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/znet)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/znetConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/znetConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/znetConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/znet)
