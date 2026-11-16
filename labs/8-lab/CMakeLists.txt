cmake_minimum_required(VERSION 3.21)

project(
	udp-pinger
	VERSION 1.0
	DESCRIPTION "UDP pinger: RTT measurement and packet loss emulation"
	LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
	message(FATAL_ERROR "The pinger uses POSIX sockets: build it on Linux or in WSL")
endif()

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(udp_pinger_server udp_pinger_server.cpp)
add_executable(udp_pinger_client udp_pinger_client.cpp)

foreach(target udp_pinger_server udp_pinger_client)
	target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
	set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endforeach()
