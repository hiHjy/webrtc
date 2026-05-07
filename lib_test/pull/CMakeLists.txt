cmake_minimum_required(VERSION 3.21)
project(srs_webrtc_pull_teaching LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../.. ${CMAKE_CURRENT_BINARY_DIR}/libdatachannel)

find_package(Threads REQUIRED)

add_executable(srs_pull main.cpp)
target_link_libraries(srs_pull PRIVATE LibDataChannel::LibDataChannel Threads::Threads)
