#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ -f work/host-env.sh ]]; then source work/host-env.sh; fi
if [[ -f work/build-env.sh ]]; then source work/build-env.sh; fi
: "${ANDROID_NDK_ROOT:?Set ANDROID_NDK_ROOT to the pinned Android NDK r30}"
python3 - "$ANDROID_NDK_ROOT" <<'PY'
import json, pathlib, sys
lock=json.loads(pathlib.Path('sources.lock.json').read_text())
properties=pathlib.Path(sys.argv[1], 'source.properties').read_text()
assert 'Pkg.Revision = '+lock['ndk_version'] in properties, 'Wrong NDK revision'
PY
export SOURCE_DATE_EPOCH=1791331200
pvb_build_dir=${1:-work/build-android}
cmake -S . -B "$pvb_build_dir" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=Release "${@:2}"
cmake --build "$pvb_build_dir" -j "${PVB_BUILD_JOBS:-4}"
