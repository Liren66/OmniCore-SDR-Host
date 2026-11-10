#!/bin/sh
# Compile check of the Windows build from macOS or Linux with mingw-w64 (brew install mingw-w64).
# It builds a copy of the working tree, so the real tree stays untouched, and gives that copy an
# empty placeholder for the bundled libusbK.dll. The result is for catching compile and link
# errors only: it is not a release build and nothing here runs the Windows binaries.
#   tools/cross-build-windows.sh [build-directory]
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-${TMPDIR:-/tmp}/och-windows-cross}
dll=third_party/driver/libusbK-3.1.0.0/libusbK-3.1.0.0-bin/bin/dll/amd64
mkdir -p "$out"
rsync -a --delete --exclude .git --exclude 'cmake-build-*' --exclude build --exclude .idea --exclude third_party \
    "$root/" "$out/src/"
mkdir -p "$out/src/$dll"
: > "$out/src/$dll/libusbK.dll"
cat > "$out/toolchain.cmake" <<'TOOLCHAIN'
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
TOOLCHAIN
cmake -S "$out/src" -B "$out/build" -DCMAKE_TOOLCHAIN_FILE="$out/toolchain.cmake" -DCMAKE_BUILD_TYPE=Release
cmake --build "$out/build" -j 6
echo "Windows cross-build OK: $out/build"
