cmake_minimum_required(VERSION 3.18)
project(zcoroutine LANGUAGES CXX)

if(NOT UNIX OR APPLE)
	message(FATAL_ERROR "zcoroutine currently supports Linux only.")
endif()

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(ZCOROUTINE_BUILD_TESTS "Build zcoroutine tests" ON)
option(ZCOROUTINE_ENABLE_LOGGING "Enable zlog based logging" ON)

set(ZCOROUTINE_SOURCES
		src/sched.cc
		src/context.cc
		src/steal_queue.cc
		src/fiber_pool.cc
		src/shared_stack_buffer.cc
		src/snapshot_buffer_pool.cc
		src/fiber_handle_registry.cc
		src/runtime_manager.cc
		src/fiber.cc
		src/timer.cc
		src/epoller.cc
		src/processor.cc
		src/event.cc
		src/mutex.cc
		src/wait_group.cc
		src/pool.cc
		src/io_event.cc
		src/hook.cc
)

add_library(zcoroutine STATIC ${ZCOROUTINE_SOURCES})

target_include_directories(zcoroutine
		PUBLIC ${PROJECT_SOURCE_DIR}/include
)

find_package(Threads REQUIRED)
target_link_libraries(zcoroutine PUBLIC Threads::Threads)

if(ZCOROUTINE_ENABLE_LOGGING)
	set(ZCOROUTINE_ZLOG_DIR "${PROJECT_SOURCE_DIR}/../zlog")
	if(NOT EXISTS "${ZCOROUTINE_ZLOG_DIR}/CMakeLists.txt")
		message(FATAL_ERROR "ZCOROUTINE_ENABLE_LOGGING=ON but zlog is not found at ${ZCOROUTINE_ZLOG_DIR}")
	endif()

	if(NOT TARGET zlog_static)
		add_subdirectory(${ZCOROUTINE_ZLOG_DIR} ${CMAKE_BINARY_DIR}/zlog)
	endif()

	target_include_directories(zcoroutine PUBLIC ${ZCOROUTINE_ZLOG_DIR}/include)
	target_link_libraries(zcoroutine PUBLIC zlog_static)
	target_compile_definitions(zcoroutine PUBLIC ZCOROUTINE_WITH_ZLOG=1)
endif()

target_compile_options(zcoroutine PRIVATE
		-Wall
		-Wextra
		-Wpedantic
)

if(ZCOROUTINE_BUILD_TESTS)
	enable_testing()
	add_subdirectory(tests)
endif()
