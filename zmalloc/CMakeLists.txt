cmake_minimum_required(VERSION 3.18)
project(zmalloc VERSION 1.0.0 LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    include(CTest)
endif()
option(BUILD_BENCHMARKS "Build benchmark executables" OFF)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

find_package(Threads REQUIRED)
option(ZMALLOC_AUTO_RELEASE "Enable bounded automatic page reclamation" OFF)

add_library(zmalloc
    src/central_cache.cc
    src/free_list.cc
    src/memory.cc
    src/page_cache.cc
    src/size_class.cc
    src/span_list.cc
    src/system_alloc.cc
    src/thread_cache.cc
    src/transfer_cache.cc
)
add_library(zmalloc::zmalloc ALIAS zmalloc)

set(zmalloc_cxx_standard 11)
target_compile_features(zmalloc PUBLIC cxx_std_${zmalloc_cxx_standard})
if(NOT CMAKE_CXX_STANDARD)
    set_property(TARGET zmalloc PROPERTY CXX_STANDARD ${zmalloc_cxx_standard})
endif()
set_target_properties(zmalloc PROPERTIES
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})
target_compile_options(zmalloc PRIVATE
    "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>"
    "$<$<CXX_COMPILER_ID:MSVC>:/W4;/permissive->")
target_include_directories(zmalloc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(zmalloc PUBLIC Threads::Threads)
target_compile_definitions(zmalloc PUBLIC
    ZMALLOC_AUTO_RELEASE_ENABLED=$<BOOL:${ZMALLOC_AUTO_RELEASE}>)

add_library(zmalloc_override STATIC src/override.cc)
add_library(zmalloc::override ALIAS zmalloc_override)
target_link_libraries(zmalloc_override PUBLIC zmalloc::zmalloc PRIVATE ${CMAKE_DL_LIBS})
if(NOT CMAKE_CXX_STANDARD)
    set_property(TARGET zmalloc_override PROPERTY CXX_STANDARD ${zmalloc_cxx_standard})
endif()
set_target_properties(zmalloc_override PROPERTIES
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
    EXPORT_NAME override)
target_compile_options(zmalloc_override PRIVATE
    "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>"
    "$<$<CXX_COMPILER_ID:MSVC>:/W4;/permissive->")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # malloc 获取自身线程局部状态时不能再次分配内存。
    target_compile_options(zmalloc PRIVATE -ftls-model=initial-exec)
    target_compile_options(zmalloc_override PRIVATE -ftls-model=initial-exec)
endif()

if(BUILD_TESTING OR BUILD_BENCHMARKS)
    add_subdirectory(tests)
endif()

install(TARGETS zmalloc zmalloc_override EXPORT zmallocTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT zmallocTargets NAMESPACE zmalloc::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zmalloc)
configure_package_config_file(cmake/zmallocConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/zmallocConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zmalloc)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/zmallocConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/zmallocConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/zmallocConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zmalloc)
