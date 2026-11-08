#!/bin/sh
# Builds artful-tests and artful-render for an Android device and runs them on it, through adb. Nothing
# is installed: the programs go to /data/local/tmp/artful, which this empties first. It changes none of
# the device's settings.
#
#   ANDROID_SERIAL=<ip>:<port> ANDROID_NDK_HOME=~/Android/Sdk/ndk/27.2.12479018 tools/androidTest.sh [abi]
#
# Then:  tools/androidTest.sh bench   measures the scenes, on both backends, with and without features
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
command -v cygpath >/dev/null 2>&1 && root=$(cygpath -m "$root") # (Git for Windows: paths adb.exe and CMake read)
export MSYS_NO_PATHCONV=1 # (Git for Windows: device paths stay as they are)
: "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME to the NDK}"
ndk=$ANDROID_NDK_HOME
command -v cygpath >/dev/null 2>&1 && ndk=$(cygpath -m "$ndk")
mode=test
abi=arm64-v8a
case "${1:-}" in bench) mode=bench ;; ?*) abi=$1 ;; esac
dir=/data/local/tmp/artful
build="$root/build-android/$abi"

cmake -S "$root" -B "$build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" -DANDROID_ABI="$abi" -DANDROID_PLATFORM=android-26 >/dev/null || exit 1
# (what failed to build must not be tested as what was built before)
cmake --build "$build" > "$build/build.log" 2>&1
built=$?
grep -E 'error|warning:|FAILED' "$build/build.log"
[ $built = 0 ] || exit 1
adb shell "rm -rf $dir && mkdir -p $dir" || exit 1
adb push "$build/artful-tests" "$build/artful-render" $dir/ >/dev/null || exit 1
adb shell "chmod 755 $dir/artful-tests $dir/artful-render"

if [ "$mode" = test ]; then
  # as it is; with one draw a mesh; with occlusion all along; with the optional features off
  for env in "" ARTFUL_SINGLE_DRAW=1 ARTFUL_FEATURES=+occlusion ARTFUL_FEATURES=-lod,-cones; do
    echo "== ${env:-defaults}"
    adb shell "cd $dir && $env ./artful-tests" | tail -n 4
  done
  exit 0
fi
adb shell "cd $dir && ./artful-render --backend software --size 320 200 | head -n 1; ./artful-render --backend vulkan --size 320 200 | head -n 1"
for scene in spheres heavy city "field --side 300"; do
  for backend in vulkan software; do
    frames=20
    [ "$backend" = vulkan ] && frames=200
    for features in -lod,-occlusion +lod,-occlusion -lod,+occlusion +lod,+occlusion; do
      printf '%-18s %-9s %-16s ' "$scene" "$backend" "$features"
      adb shell "cd $dir && ARTFUL_FEATURES=$features ./artful-render --backend $backend --scene $scene --size 1920 1080 --frames $frames" | grep 'frames:' | sed -E 's/.*: ([0-9.]+) ms a frame.*/\1/'
    done
  done
done
