cmake_minimum_required(VERSION 3.16)

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(_bashrc_file "$ENV{HOME}/.bashrc")
set(_bashrc_content "")
if(EXISTS "${_bashrc_file}")
    file(READ "${_bashrc_file}" _bashrc_content)
endif()

set(_bashrc_HOME "$ENV{HOME}")

function(_read_bashrc_export var out_var)
    if(NOT _bashrc_content)
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()

    string(REGEX MATCH "export[ \t]+${var}=\"([^\"]*)\"" _match "${_bashrc_content}")
    set(_value "${CMAKE_MATCH_1}")

    if(_value)
        foreach(_name HOME SDK RK_HOST_ROOT RKMPP_SYSROOT RK_TOOLCHAIN_BIN SYSROOT PKG_CONFIG_PATH GCC_COMPILER CXX_COMPILER)
            if(DEFINED _bashrc_${_name} AND NOT "${_bashrc_${_name}}" STREQUAL "")
                string(REPLACE "$${_name}" "${_bashrc_${_name}}" _value "${_value}")
                string(REPLACE "\${${_name}}" "${_bashrc_${_name}}" _value "${_value}")
            endif()
        endforeach()
    endif()

    set(${out_var} "${_value}" PARENT_SCOPE)
endfunction()

_read_bashrc_export(SDK _bashrc_SDK)
_read_bashrc_export(RK_HOST_ROOT _bashrc_RK_HOST_ROOT)
_read_bashrc_export(RKMPP_SYSROOT _bashrc_RKMPP_SYSROOT)
_read_bashrc_export(RK_TOOLCHAIN_BIN _bashrc_RK_TOOLCHAIN_BIN)
_read_bashrc_export(SYSROOT _bashrc_SYSROOT)
_read_bashrc_export(PKG_CONFIG_PATH _bashrc_PKG_CONFIG_PATH)
_read_bashrc_export(GCC_COMPILER _bashrc_GCC_COMPILER)
_read_bashrc_export(CXX_COMPILER _bashrc_CXX_COMPILER)

set(WEBRTC_AARCH64_SYSROOT "$ENV{SYSROOT}" CACHE PATH "aarch64 sysroot path")
if("${WEBRTC_AARCH64_SYSROOT}" STREQUAL "")
    set(WEBRTC_AARCH64_SYSROOT "${_bashrc_SYSROOT}" CACHE PATH "aarch64 sysroot path" FORCE)
endif()

set(WEBRTC_AARCH64_C_COMPILER "$ENV{GCC_COMPILER}" CACHE FILEPATH "aarch64 C compiler")
if("${WEBRTC_AARCH64_C_COMPILER}" STREQUAL "")
    set(WEBRTC_AARCH64_C_COMPILER "${_bashrc_GCC_COMPILER}" CACHE FILEPATH "aarch64 C compiler" FORCE)
endif()

set(WEBRTC_AARCH64_CXX_COMPILER "$ENV{CXX_COMPILER}" CACHE FILEPATH "aarch64 C++ compiler")
if("${WEBRTC_AARCH64_CXX_COMPILER}" STREQUAL "")
    set(WEBRTC_AARCH64_CXX_COMPILER "${_bashrc_CXX_COMPILER}" CACHE FILEPATH "aarch64 C++ compiler" FORCE)
endif()

set(WEBRTC_PKG_CONFIG_PATH "$ENV{PKG_CONFIG_PATH}" CACHE STRING "pkg-config search path")
if("${WEBRTC_PKG_CONFIG_PATH}" STREQUAL "")
    set(WEBRTC_PKG_CONFIG_PATH "${_bashrc_PKG_CONFIG_PATH}" CACHE STRING "pkg-config search path" FORCE)
endif()

if(NOT WEBRTC_AARCH64_C_COMPILER OR NOT EXISTS "${WEBRTC_AARCH64_C_COMPILER}")
    message(FATAL_ERROR
        "Cannot find aarch64 C compiler.\n"
        "Expected from ~/.bashrc GCC_COMPILER, got: ${WEBRTC_AARCH64_C_COMPILER}"
    )
endif()

if(NOT WEBRTC_AARCH64_CXX_COMPILER OR NOT EXISTS "${WEBRTC_AARCH64_CXX_COMPILER}")
    message(FATAL_ERROR
        "Cannot find aarch64 C++ compiler.\n"
        "Expected from ~/.bashrc CXX_COMPILER, got: ${WEBRTC_AARCH64_CXX_COMPILER}"
    )
endif()

if(NOT WEBRTC_AARCH64_SYSROOT OR NOT IS_DIRECTORY "${WEBRTC_AARCH64_SYSROOT}")
    message(FATAL_ERROR
        "Cannot find aarch64 sysroot.\n"
        "Expected from ~/.bashrc SYSROOT, got: ${WEBRTC_AARCH64_SYSROOT}"
    )
endif()

set(CMAKE_C_COMPILER "${WEBRTC_AARCH64_C_COMPILER}")
set(CMAKE_CXX_COMPILER "${WEBRTC_AARCH64_CXX_COMPILER}")
set(CMAKE_SYSROOT "${WEBRTC_AARCH64_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH "${WEBRTC_AARCH64_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(ENV{PKG_CONFIG_SYSROOT_DIR} "${WEBRTC_AARCH64_SYSROOT}")
set(ENV{PKG_CONFIG_PATH} "${WEBRTC_PKG_CONFIG_PATH}")

project(webrtc LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)
pkg_check_modules(RKMPP REQUIRED IMPORTED_TARGET rockchip_mpp)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat
    libavcodec
    libavutil
    libswscale
    libswresample
)
find_path(NLOHMANN_JSON_INCLUDE_DIR
    nlohmann/json.hpp
    PATHS
        /usr/include
        /usr/local/include
    NO_CMAKE_FIND_ROOT_PATH
)

if(NOT NLOHMANN_JSON_INCLUDE_DIR)
    message(FATAL_ERROR "Cannot find nlohmann/json.hpp on host include path")
endif()

set(WEBRTC_SYSROOT_INCLUDE_DIR "${CMAKE_SYSROOT}/usr/include")
set(WEBRTC_ROCKCHIP_INCLUDE_DIR "${CMAKE_SYSROOT}/usr/include/rockchip")
set(WEBRTC_FFMPEG_INCLUDE_DIR "${CMAKE_SYSROOT}/opt/ffmpeg61/include")
set(WEBRTC_PREBUILT_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/aarch64")
set(WEBRTC_PREBUILT_LIBDATACHANNEL "${WEBRTC_PREBUILT_LIB_DIR}/libdatachannel.so.0.24.2")

if(NOT EXISTS "${WEBRTC_PREBUILT_LIBDATACHANNEL}")
    message(FATAL_ERROR "Missing prebuilt libdatachannel: ${WEBRTC_PREBUILT_LIBDATACHANNEL}")
endif()

add_library(libdatachannel SHARED IMPORTED GLOBAL)
set_target_properties(libdatachannel PROPERTIES
    IMPORTED_LOCATION "${WEBRTC_PREBUILT_LIBDATACHANNEL}"
)

file(GLOB SRC_C_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
file(GLOB SRC_CXX_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)

set(WEBRTC_MAIN_SOURCE "")
foreach(_candidate
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cxx"
)
    if(EXISTS "${_candidate}")
        file(SIZE "${_candidate}" _candidate_size)
        if(_candidate_size GREATER 0)
            set(WEBRTC_MAIN_SOURCE "${_candidate}")
            break()
        endif()
    endif()
endforeach()

set(WEBRTC_LIBRARY_SOURCES "")
foreach(_src IN LISTS SRC_C_FILES SRC_CXX_FILES)
    file(SIZE "${_src}" _src_size)
    if(_src_size EQUAL 0)
        continue()
    endif()
    if(WEBRTC_MAIN_SOURCE AND _src STREQUAL WEBRTC_MAIN_SOURCE)
        continue()
    endif()
    list(APPEND WEBRTC_LIBRARY_SOURCES "${_src}")
endforeach()

if(NOT WEBRTC_LIBRARY_SOURCES AND NOT WEBRTC_MAIN_SOURCE)
    message(FATAL_ERROR "No non-empty source files found under src/")
endif()

if(WEBRTC_LIBRARY_SOURCES)
    add_library(webrtc_src STATIC ${WEBRTC_LIBRARY_SOURCES})

    target_include_directories(webrtc_src
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/include/rkmpp"
            "${WEBRTC_SYSROOT_INCLUDE_DIR}"
            "${WEBRTC_ROCKCHIP_INCLUDE_DIR}"
            "${WEBRTC_FFMPEG_INCLUDE_DIR}"
            "${NLOHMANN_JSON_INCLUDE_DIR}"
    )

    target_link_libraries(webrtc_src
        PUBLIC
            libdatachannel
            PkgConfig::LIBDRM
            PkgConfig::RKMPP
            PkgConfig::FFMPEG
            Threads::Threads
            dl
            m
    )

    target_compile_options(webrtc_src PRIVATE
        -Wall
        -Wextra
    )
endif()

if(WEBRTC_MAIN_SOURCE)
    add_executable(webrtc_app "${WEBRTC_MAIN_SOURCE}")

    target_include_directories(webrtc_app
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/include/rkmpp"
            "${WEBRTC_SYSROOT_INCLUDE_DIR}"
            "${WEBRTC_ROCKCHIP_INCLUDE_DIR}"
            "${WEBRTC_FFMPEG_INCLUDE_DIR}"
            "${NLOHMANN_JSON_INCLUDE_DIR}"
    )

    if(TARGET webrtc_src)
        target_link_libraries(webrtc_app PRIVATE webrtc_src)
    else()
        target_link_libraries(webrtc_app
            PRIVATE
                libdatachannel
                PkgConfig::LIBDRM
                PkgConfig::RKMPP
                PkgConfig::FFMPEG
                Threads::Threads
                dl
                m
        )
    endif()

    set_target_properties(webrtc_app PROPERTIES
        BUILD_RPATH "$ORIGIN/../lib/aarch64"
        INSTALL_RPATH "$ORIGIN/../lib/aarch64"
    )
endif()

message(STATUS "Using C compiler   : ${CMAKE_C_COMPILER}")
message(STATUS "Using CXX compiler : ${CMAKE_CXX_COMPILER}")
message(STATUS "Using sysroot      : ${CMAKE_SYSROOT}")
message(STATUS "Using PKG_CONFIG   : $ENV{PKG_CONFIG_PATH}")
message(STATUS "Main source        : ${WEBRTC_MAIN_SOURCE}")
