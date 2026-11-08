cmake_minimum_required(VERSION 3.15)

project(reliable-udp-arq
    VERSION 1.0.0
    DESCRIPTION "Educational reliable data transfer (Stop-and-Wait / Go-Back-N ARQ) over UDP"
    LANGUAGES CXX)

if(WIN32 AND NOT CYGWIN)
    message(FATAL_ERROR
        "This project uses POSIX Berkeley sockets (sys/socket.h). "
        "Please build it on Linux, macOS or WSL.")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Core library ------------------------------------------------------------
add_library(rudp STATIC src/ReliableSocket.cpp)
target_include_directories(rudp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(rudp PUBLIC cxx_std_17)
target_compile_options(rudp PUBLIC
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Wshadow -Wconversion>)

# --- Applications ------------------------------------------------------------
add_executable(sender src/sender.cpp)
target_link_libraries(sender PRIVATE rudp)

add_executable(receiver src/receiver.cpp)
target_link_libraries(receiver PRIVATE rudp)
