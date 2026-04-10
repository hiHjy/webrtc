cmake_minimum_required(VERSION 3.21)
project(srs_webrtc_push_teaching LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../.. ${CMAKE_CURRENT_BINARY_DIR}/libdatachannel)

find_package(Threads REQUIRED)

add_executable(srs_push_file main_push_file.cpp)
target_link_libraries(srs_push_file PRIVATE LibDataChannel::LibDataChannel Threads::Threads)

add_executable(srs_push_callback main_push_callback.cpp)
target_link_libraries(srs_push_callback PRIVATE LibDataChannel::LibDataChannel Threads::Threads)
