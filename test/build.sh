#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

arch="${WEBRTC_BUILD_ARCH:-x86}"
if [[ $# -gt 0 ]]; then
    case "$1" in
        x86|aarch64|all)
            arch="$1"
            shift
            ;;
    esac
fi

build_one() {
    local target_arch="$1"
    shift
    local build_dir="build-${target_arch}"
    local -a cmake_args=(
        -S .
        -B "${build_dir}"
        -DCMAKE_BUILD_TYPE=Debug
        -DWEBRTC_TARGET_ARCH="${target_arch}"
    )

    if [[ "${target_arch}" == "aarch64" ]]; then
        local sysroot="${SYSROOT:-$HOME/rk3568_sysroot_fixed}"
        export SYSROOT="${sysroot}"
        export PKG_CONFIG_SYSROOT_DIR="${sysroot}"
        export PKG_CONFIG_LIBDIR="${sysroot}/usr/lib/aarch64-linux-gnu/pkgconfig:${sysroot}/usr/lib/pkgconfig:${sysroot}/usr/share/pkgconfig"
        unset PKG_CONFIG_PATH

        cmake_args+=(
            -DWEBRTC_SYSROOT="${sysroot}"
            -DCMAKE_C_COMPILER="${CC:-aarch64-linux-gnu-gcc}"
            -DCMAKE_CXX_COMPILER="${CXX:-aarch64-linux-gnu-g++}"
        )
    fi

    cmake "${cmake_args[@]}" "$@"
    cmake --build "${build_dir}" -j"$(nproc)"
}

case "${arch}" in
    x86)
        build_one x86 "$@"
        cmake -E create_symlink build-x86/compile_commands.json compile_commands.json
        ;;
    aarch64)
        build_one aarch64 "$@"
        cmake -E create_symlink build-aarch64/compile_commands.json compile_commands.json
        ;;
    all)
        build_one x86 "$@"
        build_one aarch64 "$@"
        cmake -E create_symlink build-x86/compile_commands.json compile_commands.json
        ;;
    *)
        echo "usage: $0 [x86|aarch64|all] [extra cmake args...]" >&2
        exit 2
        ;;
esac
