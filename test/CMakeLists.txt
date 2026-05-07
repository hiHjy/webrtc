cmake_minimum_required(VERSION 3.16)

set(WEBRTC_TARGET_ARCH "x86" CACHE STRING "Build target architecture")
set_property(CACHE WEBRTC_TARGET_ARCH PROPERTY STRINGS x86 aarch64)

if(DEFINED WEBRTC_SYSROOT AND NOT "${WEBRTC_SYSROOT}" STREQUAL "")
    set(WEBRTC_SYSROOT "${WEBRTC_SYSROOT}" CACHE PATH "Target sysroot")
elseif(DEFINED ENV{SYSROOT} AND NOT "$ENV{SYSROOT}" STREQUAL "")
    set(WEBRTC_SYSROOT "$ENV{SYSROOT}" CACHE PATH "Target sysroot")
elseif(EXISTS "$ENV{HOME}/rk3568_sysroot_fixed")
    set(WEBRTC_SYSROOT "$ENV{HOME}/rk3568_sysroot_fixed" CACHE PATH "Target sysroot")
else()
    set(WEBRTC_SYSROOT "" CACHE PATH "Target sysroot")
endif()

if(WEBRTC_TARGET_ARCH STREQUAL "aarch64")
    set(CMAKE_SYSTEM_NAME Linux CACHE STRING "" FORCE)
    set(CMAKE_SYSTEM_PROCESSOR aarch64 CACHE STRING "" FORCE)

    if(WEBRTC_SYSROOT)
        set(CMAKE_SYSROOT "${WEBRTC_SYSROOT}" CACHE PATH "" FORCE)
        set(CMAKE_FIND_ROOT_PATH "${WEBRTC_SYSROOT}" CACHE STRING "" FORCE)
        set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
        set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
        set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
        set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
    endif()

    if(NOT DEFINED CMAKE_C_COMPILER)
        if(DEFINED ENV{CC} AND NOT "$ENV{CC}" STREQUAL "")
            set(CMAKE_C_COMPILER "$ENV{CC}" CACHE FILEPATH "" FORCE)
        else()
            find_program(AARCH64_GCC aarch64-linux-gnu-gcc)
            if(AARCH64_GCC)
                set(CMAKE_C_COMPILER "${AARCH64_GCC}" CACHE FILEPATH "" FORCE)
            endif()
        endif()
    endif()

    if(NOT DEFINED CMAKE_CXX_COMPILER)
        if(DEFINED ENV{CXX} AND NOT "$ENV{CXX}" STREQUAL "")
            set(CMAKE_CXX_COMPILER "$ENV{CXX}" CACHE FILEPATH "" FORCE)
        else()
            find_program(AARCH64_GXX aarch64-linux-gnu-g++)
            if(AARCH64_GXX)
                set(CMAKE_CXX_COMPILER "${AARCH64_GXX}" CACHE FILEPATH "" FORCE)
            endif()
        endif()
    endif()
endif()

project(webrtc_test LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

get_filename_component(WEBRTC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(WEBRTC_INCLUDE_DIR "${WEBRTC_ROOT}/include")
set(LIBDATACHANNEL_INCLUDE_DIR "${WEBRTC_ROOT}/libdatachannel/include")
set(JSON_INCLUDE_DIR "${WEBRTC_ROOT}/libdatachannel/deps/json/include")
set(PREBUILT_LIB_DIR "${WEBRTC_ROOT}/lib/${WEBRTC_TARGET_ARCH}")
set(PREBUILT_LIBDATACHANNEL "${PREBUILT_LIB_DIR}/libdatachannel.so.0.24.2")

if(NOT EXISTS "${PREBUILT_LIBDATACHANNEL}")
    message(FATAL_ERROR "Missing libdatachannel: ${PREBUILT_LIBDATACHANNEL}")
endif()

add_library(datachannel SHARED IMPORTED GLOBAL)
set_target_properties(datachannel PROPERTIES
    IMPORTED_LOCATION "${PREBUILT_LIBDATACHANNEL}"
    INTERFACE_INCLUDE_DIRECTORIES "${LIBDATACHANNEL_INCLUDE_DIR}"
)

add_library(webrtc_test_common STATIC
    "${WEBRTC_ROOT}/src/config_reader.cpp"
    "${WEBRTC_ROOT}/src/srs_api.cpp"
    "${WEBRTC_ROOT}/src/webrtctosrs.cpp"
    "${WEBRTC_ROOT}/src/srstowebrtc.cpp"
)

target_include_directories(webrtc_test_common PUBLIC
    "${WEBRTC_INCLUDE_DIR}"
    "${LIBDATACHANNEL_INCLUDE_DIR}"
    "${JSON_INCLUDE_DIR}"
)

target_compile_definitions(webrtc_test_common PUBLIC
    WEBRTC_PROJECT_ROOT="${WEBRTC_ROOT}"
)

target_link_libraries(webrtc_test_common PUBLIC
    datachannel
    pthread
    dl
)

set(TEST_COMMON_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/cam"
    "${CMAKE_CURRENT_SOURCE_DIR}/decoder"
    "${CMAKE_CURRENT_SOURCE_DIR}/drm"
    "${CMAKE_CURRENT_SOURCE_DIR}/encoder"
    "${CMAKE_CURRENT_SOURCE_DIR}/audio_input/include"
)

set(LOCAL_HINT_INCLUDE_DIRS
    "$ENV{HOME}/nfs/cam_zero_copy-mpp_test"
    "$ENV{HOME}/git/dma-buf"
)

foreach(dir IN LISTS LOCAL_HINT_INCLUDE_DIRS)
    if(EXISTS "${dir}")
        list(APPEND TEST_COMMON_INCLUDE_DIRS "${dir}")
    endif()
endforeach()

if(WEBRTC_SYSROOT)
    list(APPEND TEST_COMMON_INCLUDE_DIRS
        "${WEBRTC_SYSROOT}/usr/include"
        "${WEBRTC_SYSROOT}/usr/include/rockchip"
        "${WEBRTC_SYSROOT}/usr/include/libdrm"
    )
endif()

find_path(HOST_LIBDRM_INCLUDE_DIR
    NAMES xf86drm.h
    PATHS /usr/include /usr/local/include
    NO_CMAKE_FIND_ROOT_PATH
)
find_path(HOST_LIBDRM_MODE_INCLUDE_DIR
    NAMES drm.h
    PATHS /usr/include/libdrm /usr/local/include/libdrm
    NO_CMAKE_FIND_ROOT_PATH
)

if(HOST_LIBDRM_INCLUDE_DIR)
    list(APPEND TEST_COMMON_INCLUDE_DIRS "${HOST_LIBDRM_INCLUDE_DIR}")
endif()
if(HOST_LIBDRM_MODE_INCLUDE_DIR)
    list(APPEND TEST_COMMON_INCLUDE_DIRS "${HOST_LIBDRM_MODE_INCLUDE_DIR}")
endif()

if(WEBRTC_TARGET_ARCH STREQUAL "x86")
    add_executable(webrtc_push_x86
        "${CMAKE_CURRENT_SOURCE_DIR}/app/push/x86/main.cpp"
    )

    target_include_directories(webrtc_push_x86 PRIVATE
        ${TEST_COMMON_INCLUDE_DIRS}
    )

    target_link_libraries(webrtc_push_x86 PRIVATE
        webrtc_test_common
    )

    set_target_properties(webrtc_push_x86 PROPERTIES
        BUILD_RPATH "$ORIGIN;${PREBUILT_LIB_DIR}"
        INSTALL_RPATH "$ORIGIN"
    )

    add_custom_command(TARGET webrtc_push_x86 POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${PREBUILT_LIBDATACHANNEL}"
                "$<TARGET_FILE_DIR:webrtc_push_x86>/libdatachannel.so.0.24.2"
        COMMAND "${CMAKE_COMMAND}" -E create_symlink
                "libdatachannel.so.0.24.2"
                "$<TARGET_FILE_DIR:webrtc_push_x86>/libdatachannel.so.0.24"
        COMMAND "${CMAKE_COMMAND}" -E create_symlink
                "libdatachannel.so.0.24"
                "$<TARGET_FILE_DIR:webrtc_push_x86>/libdatachannel.so"
    )
endif()

if(WEBRTC_TARGET_ARCH STREQUAL "aarch64")
    set(TARGET_LIB_DIR "")
    if(WEBRTC_SYSROOT)
        set(TARGET_LIB_DIR "${WEBRTC_SYSROOT}/usr/lib/aarch64-linux-gnu")
    endif()

    set(SSL_LIBRARY ssl)
    set(CRYPTO_LIBRARY crypto)
    set(OPUS_LIBRARY opus)
    set(ASOUND_LIBRARY asound)
    if(TARGET_LIB_DIR)
        if(EXISTS "${TARGET_LIB_DIR}/libssl.so.1.1")
            set(SSL_LIBRARY "${TARGET_LIB_DIR}/libssl.so.1.1")
        endif()
        if(EXISTS "${TARGET_LIB_DIR}/libcrypto.so.1.1")
            set(CRYPTO_LIBRARY "${TARGET_LIB_DIR}/libcrypto.so.1.1")
        endif()
        if(EXISTS "${TARGET_LIB_DIR}/libopus.so")
            set(OPUS_LIBRARY "${TARGET_LIB_DIR}/libopus.so")
        elseif(EXISTS "${TARGET_LIB_DIR}/libopus.so.0")
            set(OPUS_LIBRARY "${TARGET_LIB_DIR}/libopus.so.0")
        endif()
        if(EXISTS "${TARGET_LIB_DIR}/libasound.so")
            set(ASOUND_LIBRARY "${TARGET_LIB_DIR}/libasound.so")
        elseif(EXISTS "${TARGET_LIB_DIR}/libasound.so.2")
            set(ASOUND_LIBRARY "${TARGET_LIB_DIR}/libasound.so.2")
        endif()
    endif()

    find_path(OPUS_INCLUDE_DIR
        NAMES opus/opus.h
        PATHS "${WEBRTC_SYSROOT}/usr/include"
    )
    if(NOT OPUS_INCLUDE_DIR)
        find_path(OPUS_INCLUDE_DIR
            NAMES opus/opus.h
            PATHS /usr/include /usr/local/include
            NO_CMAKE_FIND_ROOT_PATH
        )
    endif()

    find_path(ALSA_INCLUDE_DIR
        NAMES alsa/asoundlib.h
        PATHS "${WEBRTC_SYSROOT}/usr/include"
    )
    if(NOT ALSA_INCLUDE_DIR)
        find_path(ALSA_INCLUDE_DIR
            NAMES alsa/asoundlib.h
            PATHS /usr/include /usr/local/include
            NO_CMAKE_FIND_ROOT_PATH
        )
    endif()

    add_executable(webrtc_test_aarch64
        "${CMAKE_CURRENT_SOURCE_DIR}/app/pull/aarch64/main.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/decoder/rkmpp_dec.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/drm/drm_test.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/audio_output/src/audio_playback.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/audio_output/src/audio_playback_manager.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/audio_output/src/opus_decoder.c"
    )

    target_include_directories(webrtc_test_aarch64 PRIVATE
        ${TEST_COMMON_INCLUDE_DIRS}
        "${CMAKE_CURRENT_SOURCE_DIR}/audio_output/include"
        "${OPUS_INCLUDE_DIR}"
        "${ALSA_INCLUDE_DIR}"
    )

    if(OPUS_INCLUDE_DIR)
        target_compile_options(webrtc_test_aarch64 PRIVATE "-I${OPUS_INCLUDE_DIR}")
    endif()
    if(ALSA_INCLUDE_DIR)
        target_compile_options(webrtc_test_aarch64 PRIVATE "-I${ALSA_INCLUDE_DIR}")
    endif()

    if(TARGET_LIB_DIR)
        target_link_directories(webrtc_test_aarch64 PRIVATE "${TARGET_LIB_DIR}")
    endif()

    target_link_libraries(webrtc_test_aarch64 PRIVATE
        webrtc_test_common
        rockchip_mpp
        drm
        "${ASOUND_LIBRARY}"
        "${OPUS_LIBRARY}"
        "${SSL_LIBRARY}"
        "${CRYPTO_LIBRARY}"
    )

    target_compile_definitions(webrtc_test_aarch64 PRIVATE
        _DEFAULT_SOURCE
    )

    set_target_properties(webrtc_test_aarch64 PROPERTIES
        BUILD_RPATH "$ORIGIN;${PREBUILT_LIB_DIR}"
        INSTALL_RPATH "$ORIGIN"
    )

    add_custom_command(TARGET webrtc_test_aarch64 POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${PREBUILT_LIBDATACHANNEL}"
                "$<TARGET_FILE_DIR:webrtc_test_aarch64>/libdatachannel.so.0.24.2"
        COMMAND "${CMAKE_COMMAND}" -E create_symlink
                "libdatachannel.so.0.24.2"
                "$<TARGET_FILE_DIR:webrtc_test_aarch64>/libdatachannel.so.0.24"
        COMMAND "${CMAKE_COMMAND}" -E create_symlink
                "libdatachannel.so.0.24"
                "$<TARGET_FILE_DIR:webrtc_test_aarch64>/libdatachannel.so"
    )

    add_executable(webrtc_push_aarch64
        "${CMAKE_CURRENT_SOURCE_DIR}/app/push/aarch64/main.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/cam/cam.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/encoder/rkmpp_enc.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/audio_input/src/audio_capture.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/audio_input/src/audio_stream_manager.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/audio_input/src/opus_encoder.c"
    )

    target_include_directories(webrtc_push_aarch64 PRIVATE
        ${TEST_COMMON_INCLUDE_DIRS}
        "${OPUS_INCLUDE_DIR}"
        "${ALSA_INCLUDE_DIR}"
    )

    if(OPUS_INCLUDE_DIR)
        target_compile_options(webrtc_push_aarch64 PRIVATE "-I${OPUS_INCLUDE_DIR}")
    endif()
    if(ALSA_INCLUDE_DIR)
        target_compile_options(webrtc_push_aarch64 PRIVATE "-I${ALSA_INCLUDE_DIR}")
    endif()

    if(TARGET_LIB_DIR)
        target_link_directories(webrtc_push_aarch64 PRIVATE "${TARGET_LIB_DIR}")
    endif()

    target_link_libraries(webrtc_push_aarch64 PRIVATE
        webrtc_test_common
        rockchip_mpp
        drm
        "${ASOUND_LIBRARY}"
        "${OPUS_LIBRARY}"
        "${SSL_LIBRARY}"
        "${CRYPTO_LIBRARY}"
    )

    target_compile_definitions(webrtc_push_aarch64 PRIVATE
        _DEFAULT_SOURCE
    )

    set_target_properties(webrtc_push_aarch64 PROPERTIES
        BUILD_RPATH "$ORIGIN;${PREBUILT_LIB_DIR}"
        INSTALL_RPATH "$ORIGIN"
    )

    add_custom_command(TARGET webrtc_push_aarch64 POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${PREBUILT_LIBDATACHANNEL}"
                "$<TARGET_FILE_DIR:webrtc_push_aarch64>/libdatachannel.so.0.24.2"
        COMMAND "${CMAKE_COMMAND}" -E create_symlink
                "libdatachannel.so.0.24.2"
                "$<TARGET_FILE_DIR:webrtc_push_aarch64>/libdatachannel.so.0.24"
        COMMAND "${CMAKE_COMMAND}" -E create_symlink
                "libdatachannel.so.0.24"
                "$<TARGET_FILE_DIR:webrtc_push_aarch64>/libdatachannel.so"
    )
endif()

message(STATUS "WEBRTC_TARGET_ARCH = ${WEBRTC_TARGET_ARCH}")
message(STATUS "WEBRTC_ROOT = ${WEBRTC_ROOT}")
message(STATUS "WEBRTC_SYSROOT = ${WEBRTC_SYSROOT}")
message(STATUS "Using libdatachannel = ${PREBUILT_LIBDATACHANNEL}")
message(STATUS "Executable output dir = ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "Code hint include dirs = ${TEST_COMMON_INCLUDE_DIRS}")
