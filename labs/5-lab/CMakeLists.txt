cmake_minimum_required(VERSION 3.16)

project(
	sockets
	VERSION 1.0
	DESCRIPTION "TCP client and server: sequential, fork and epoll + thread pool"
	LANGUAGES C)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
	message(FATAL_ERROR "The server uses epoll, signalfd and fork: build it on Linux or in WSL")
endif()

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build" FORCE)
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(STUDENT_NAME "Ivan Petrov" CACHE STRING "Name used in \"Client of ...\" and \"Server of ...\"")

find_package(Threads REQUIRED)

foreach(target client server)
	add_executable(${target} ${target}.c)
	target_compile_definitions(${target} PRIVATE STUDENT_NAME="${STUDENT_NAME}")
	target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror)
	set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}")
endforeach()

target_link_libraries(server PRIVATE Threads::Threads)
