cmake_minimum_required(VERSION 3.21)

project(
	webserver
	VERSION 1.0
	DESCRIPTION "Static HTTP server: sequential, threaded and epoll modes"
	LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
	message(FATAL_ERROR "The server uses epoll and signalfd: build it on Linux or in WSL")
endif()

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(
	survey
	GIT_REPOSITORY https://github.com/lconantl/survey.git
	GIT_TAG v1.0.0
	GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(survey)

find_package(Threads REQUIRED)

add_executable(webserver webserver.cpp)
target_link_libraries(webserver PRIVATE survey::survey Threads::Threads)
target_compile_options(webserver PRIVATE -Wall -Wextra -Wpedantic -Werror)

set_target_properties(webserver PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}")
