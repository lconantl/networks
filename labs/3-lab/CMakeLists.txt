cmake_minimum_required(VERSION 3.21)

project(
        smtp-client-wsl
	VERSION 1.0
	DESCRIPTION "SMTP client: STARTTLS, TLS, AUTH, MIME attachments"
	LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SMTP_CLIENT_WITH_TLS "Use OpenSSL for STARTTLS and TLS" ON)

include(FetchContent)
FetchContent_Declare(
	survey
	GIT_REPOSITORY https://github.com/lconantl/survey.git
	GIT_TAG v1.0.0
	GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(survey)

add_executable(smtp-client smtp-client.cpp)
target_link_libraries(smtp-client PRIVATE survey::survey)

if(WIN32)
	target_compile_definitions(smtp-client PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
	target_link_libraries(smtp-client PRIVATE ws2_32 crypt32)
endif()

if(SMTP_CLIENT_WITH_TLS)
	if(MINGW)
		set(OPENSSL_USE_STATIC_LIBS TRUE)
	endif()

	find_package(OpenSSL 3.0)

	if(OpenSSL_FOUND)
		target_link_libraries(smtp-client PRIVATE OpenSSL::SSL OpenSSL::Crypto)
		target_compile_definitions(smtp-client PRIVATE SMTP_CLIENT_HAS_TLS)
		message(STATUS "OpenSSL ${OPENSSL_VERSION}: STARTTLS and TLS enabled")
	else()
		message(WARNING "OpenSSL not found: building without STARTTLS/TLS")
	endif()
endif()

if(MSVC)
	target_compile_options(smtp-client PRIVATE /utf-8 /EHsc /Zc:__cplusplus /W4 /WX)
else()
	target_compile_options(smtp-client PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(MINGW)
	target_link_options(smtp-client PRIVATE -static)
endif()

set_target_properties(smtp-client PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}")
