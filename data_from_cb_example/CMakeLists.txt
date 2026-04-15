cmake_minimum_required(VERSION 3.16)

project(webrtc_to_srs LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 默认使用本机 x86 版本库。
# 切到 aarch64 时，改这里或在命令行传:
#   cmake -S . -B build -DWEBRTC_TARGET_ARCH=aarch64
set(WEBRTC_TARGET_ARCH "x86" CACHE STRING "Prebuilt libdatachannel architecture")
set_property(CACHE WEBRTC_TARGET_ARCH PROPERTY STRINGS x86 aarch64)

set(PROJECT_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(PROJECT_INCLUDE_DIR "${PROJECT_ROOT_DIR}/include")
set(LIBDATACHANNEL_INCLUDE_DIR "${PROJECT_ROOT_DIR}/libdatachannel/include")
set(JSON_INCLUDE_DIR "${PROJECT_ROOT_DIR}/libdatachannel/deps/json/include")
set(PREBUILT_LIB_DIR "${PROJECT_ROOT_DIR}/lib/${WEBRTC_TARGET_ARCH}")
set(PREBUILT_LIBDATACHANNEL "${PREBUILT_LIB_DIR}/libdatachannel.so.0.24.2")

if(NOT EXISTS "${PREBUILT_LIBDATACHANNEL}")
    message(FATAL_ERROR
        "Missing prebuilt libdatachannel: ${PREBUILT_LIBDATACHANNEL}\n"
        "Please check WEBRTC_TARGET_ARCH or place the library under lib/<arch>/."
    )
endif()

# 让可执行程序稳定落到 build 目录，后续你手动整理代码时也比较直观。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

add_library(libdatachannel SHARED IMPORTED GLOBAL)
set_target_properties(libdatachannel PROPERTIES
    IMPORTED_LOCATION "${PREBUILT_LIBDATACHANNEL}"
    INTERFACE_INCLUDE_DIRECTORIES "${LIBDATACHANNEL_INCLUDE_DIR}"
)

add_library(webrtc_app_common STATIC
    "${PROJECT_ROOT_DIR}/src/config_reader.cpp"
    "${PROJECT_ROOT_DIR}/src/srs_api.cpp"
    "${PROJECT_ROOT_DIR}/src/webrtctosrs.cpp"
    "${PROJECT_ROOT_DIR}/src/srstowebrtc.cpp"
)

target_include_directories(webrtc_app_common PRIVATE
    "${PROJECT_INCLUDE_DIR}"
    "${LIBDATACHANNEL_INCLUDE_DIR}"
    "${JSON_INCLUDE_DIR}"
)

target_compile_definitions(webrtc_app_common PRIVATE
    WEBRTC_PROJECT_ROOT="${PROJECT_ROOT_DIR}"
)

target_link_libraries(webrtc_app_common PRIVATE
    libdatachannel
    pthread
)

add_executable(webrtc_to_srs_app "${PROJECT_ROOT_DIR}/src/main_push.cpp")
add_executable(srs_to_webrtc_app "${PROJECT_ROOT_DIR}/src/main_pull.cpp")

target_link_libraries(webrtc_to_srs_app PRIVATE webrtc_app_common)
target_link_libraries(srs_to_webrtc_app PRIVATE webrtc_app_common)

target_include_directories(webrtc_to_srs_app PRIVATE
    "${PROJECT_INCLUDE_DIR}"
)
target_include_directories(srs_to_webrtc_app PRIVATE
    "${PROJECT_INCLUDE_DIR}"
)

target_compile_definitions(webrtc_to_srs_app PRIVATE
    WEBRTC_PROJECT_ROOT="${PROJECT_ROOT_DIR}"
)
target_compile_definitions(srs_to_webrtc_app PRIVATE
    WEBRTC_PROJECT_ROOT="${PROJECT_ROOT_DIR}"
)

# 运行时优先从当前选中的预编译库目录找动态库。
set_target_properties(webrtc_app_common PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
set_target_properties(webrtc_to_srs_app PROPERTIES
    BUILD_RPATH "${PREBUILT_LIB_DIR}"
    INSTALL_RPATH "${PREBUILT_LIB_DIR}"
)
set_target_properties(srs_to_webrtc_app PROPERTIES
    BUILD_RPATH "${PREBUILT_LIB_DIR}"
    INSTALL_RPATH "${PREBUILT_LIB_DIR}"
)

message(STATUS "WEBRTC_TARGET_ARCH = ${WEBRTC_TARGET_ARCH}")
message(STATUS "Using libdatachannel = ${PREBUILT_LIBDATACHANNEL}")
message(STATUS "Executable output dir = ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
