#!/bin/sh
# All tests under AddressSanitizer and UndefinedBehaviorSanitizer, as CI's "Memory" step runs them, and with what is
# still allocated counted (ARTFUL_COUNT_ALLOCATIONS: LeakSanitizer does not run on Windows). Clang, Debug.
#
#   tools/sanitize.sh [build dir] [test words...]
#
# On Windows, from MSYS2's clang64 (pacman -S mingw-w64-clang-x86_64-clang mingw-w64-clang-x86_64-compiler-rt
# mingw-w64-clang-x86_64-cmake mingw-w64-clang-x86_64-ninja), with C:/msys64/clang64/bin first in PATH. There the
# stack is a megabyte unless the program asks for more, and unoptimized kernels with ASan's guards around each local
# take about one (lightLanes; optimized, a few kilobytes): the tests ask for 32.
# The CPU's kernels are each tested at every instruction set within the tests (ARTF_SIMD_...); Vulkan runs where a
# driver is found.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-"$root/build-sanitize"}
[ $# -gt 0 ] && shift
flags="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-sanitize=float-cast-overflow -DARTFUL_COUNT_ALLOCATIONS"
link=""
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) link="-Wl,--stack,33554432" ;; esac
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug "-DCMAKE_C_FLAGS=$flags" "-DCMAKE_EXE_LINKER_FLAGS=$link"
cmake --build "$build" --target artful-tests
"$build/artful-tests" "$@"
ARTFUL_FEATURES=+occlusion "$build/artful-tests" "$@"
