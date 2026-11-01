cmake_minimum_required(VERSION 3.21)

project(
	dns-resolver
	VERSION 1.0
	DESCRIPTION "Iterative DNS resolver: UDP, TCP fallback, TTL cache"
	LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
	message(FATAL_ERROR "The resolver uses POSIX sockets: build it on Linux or in WSL")
endif()

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(dns_resolver dns_resolver.cpp)
target_compile_options(dns_resolver PRIVATE -Wall -Wextra -Wpedantic -Werror)

set_target_properties(dns_resolver PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
