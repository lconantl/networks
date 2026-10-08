cmake_minimum_required(VERSION 3.28)
project(smtp VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

file(GLOB_RECURSE CORE_FILES CONFIGURE_DEPENDS "*.cpp")
list(REMOVE_ITEM CORE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")
add_library(core STATIC ${CORE_FILES})
target_include_directories(core PUBLIC src)

if(MSVC)
    target_compile_options(core PUBLIC /W4 /utf-8 /permissive-)
    target_compile_definitions(core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(core PUBLIC -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(smtp main.cpp)
target_link_libraries(smtp PRIVATE core)