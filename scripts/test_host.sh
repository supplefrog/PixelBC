#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ -f work/host-env.sh ]]; then source work/host-env.sh; fi
if [[ -f work/build-env.sh ]]; then source work/build-env.sh; fi
pvb_host_build=${1:-work/build-host}
pvb_cmake_args=()
if [[ -d work/host-root/usr/include/vulkan ]]; then pvb_cmake_args+=(-DVULKAN_HEADERS_DIR="$PWD/work/host-root/usr/include"); fi
cmake -S . -B "$pvb_host_build" -G Ninja -DCMAKE_BUILD_TYPE=Release "${pvb_cmake_args[@]}"
cmake --build "$pvb_host_build" -j "${PVB_BUILD_JOBS:-4}"
pvb_host_absolute=$(python3 -c 'import pathlib,sys;print(pathlib.Path(sys.argv[1]).resolve())' "$pvb_host_build")
"$pvb_host_absolute/pixel_bc_probe" "$pvb_host_absolute/libvulkan_pixel_bc.so" --validation
ctest --test-dir "$pvb_host_build" --output-on-failure
