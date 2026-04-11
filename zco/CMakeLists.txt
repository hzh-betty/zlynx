cmake_minimum_required(VERSION 3.18)
project(zco LANGUAGES CXX)

if(NOT UNIX OR APPLE)
	message(FATAL_ERROR "zco currently supports Linux only.")
endif()

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(ZCO_BUILD_TESTS "Build zco tests" ON)

set(ZCO_SOURCES
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

set(ZCO_ZLOG_DIR "${PROJECT_SOURCE_DIR}/../zlog")
if(NOT EXISTS "${ZCO_ZLOG_DIR}/CMakeLists.txt")
	message(FATAL_ERROR "zlog is not found at ${ZCO_ZLOG_DIR}")
endif()

if(NOT TARGET zlog_static)
	add_subdirectory(${ZCO_ZLOG_DIR} ${CMAKE_BINARY_DIR}/zlog)
endif()

function(configure_zco_target target_name)
	target_include_directories(${target_name}
			PUBLIC
				${PROJECT_SOURCE_DIR}/include
				${ZCO_ZLOG_DIR}/include
	)

	find_package(Threads REQUIRED)
	target_link_libraries(${target_name} PUBLIC Threads::Threads zlog_static)

	target_compile_options(${target_name} PRIVATE
			-Wall
			-Wextra
			-Wpedantic
	)

	if(ENABLE_COVERAGE)
		if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
			target_compile_options(${target_name} PRIVATE -O0 -g --coverage)
			target_link_options(${target_name} PUBLIC --coverage)
		else()
			message(WARNING "ENABLE_COVERAGE is ON, but compiler ${CMAKE_CXX_COMPILER_ID} is not supported")
		endif()
	endif()
endfunction()

add_library(zco STATIC ${ZCO_SOURCES})
configure_zco_target(zco)

if(ZCO_BUILD_TESTS)
	add_library(zco_stdalloc STATIC ${ZCO_SOURCES})
	configure_zco_target(zco_stdalloc)
endif()

if(ZCO_BUILD_TESTS)
	enable_testing()
	add_subdirectory(tests)
endif()
