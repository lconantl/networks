cmake_minimum_required(VERSION 3.16)
project(rdtp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(rdtp_core STATIC
    src/Channel.cpp
    src/FileTransfer.cpp
    src/Logger.cpp
    src/Options.cpp
    src/Packet.cpp
    src/Receiver.cpp
    src/Sender.cpp
    src/UdpSocket.cpp
)

target_include_directories(rdtp_core PUBLIC include)
target_compile_options(rdtp_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(rdt_sender src/rdt_sender.cpp)
add_executable(rdt_receiver src/rdt_receiver.cpp)

target_link_libraries(rdt_sender PRIVATE rdtp_core)
target_link_libraries(rdt_receiver PRIVATE rdtp_core)
