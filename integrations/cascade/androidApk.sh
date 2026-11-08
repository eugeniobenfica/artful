#!/bin/sh
# Builds integrations/cascade/example as an Android APK: a page of cascade over a scene of Artful, in a real window,
# with no Gradle and no JDK. The Java part is cascade's own activity, already compiled by its
# examples/android/buildApk.sh (classes.dex). Without a JDK apksigner cannot run, so signApk.py, beside this,
# signs it, with a key of its own: enough for tests, not for a store.
#
#   ANDROID_HOME=~/Android/Sdk ANDROID_NDK_HOME=~/Android/Sdk/ndk/27.2.12479018 CASCADE=../Cascade integrations/cascade/androidApk.sh
#   adb install -r build-android/artful-cascade.apk
set -e
: "${ANDROID_HOME:?set ANDROID_HOME to the Android SDK}"
: "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME to the NDK}"
: "${CASCADE:?set CASCADE to cascade's source folder}"
ABI=${ABI:-arm64-v8a}
root=$(cd "$(dirname "$0")/../.." && pwd)
here="$root/integrations/cascade"
out="$root/build-android"
win() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else echo "$1"; fi; }
tools=$(ls -d "$ANDROID_HOME"/build-tools/* | sort -V | tail -n 1)
platform=$(ls -d "$ANDROID_HOME"/platforms/android-* | sort -V | tail -n 1)
dex="$CASCADE/build-android/stage/classes.dex"
[ -f "$dex" ] || { echo "no $dex: run cascade's examples/android/buildApk.sh once"; exit 1; }

cmake -S "$(win "$root")" -B "$(win "$out/app-$ABI")" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$(win "$ANDROID_NDK_HOME")/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI="$ABI" -DANDROID_PLATFORM=android-26 -DCMAKE_BUILD_TYPE=Release -DARTFUL_CASCADE_DIR="$(win "$CASCADE")" -DARTFUL_BUILD_TOOLS=OFF -DARTFUL_BUILD_TESTS=OFF "-DCMAKE_C_FLAGS=${CFLAGS:-}" >/dev/null 2>&1
cmake --build "$(win "$out/app-$ABI")" --target artful-cascade 2>&1 | grep -E ' error |FAILED' && exit 1

stage="$out/app-stage"
rm -rf "$stage" && mkdir -p "$stage/assets" "$stage/lib/$ABI"
cp "$out/app-$ABI/libartful-cascade.so" "$stage/lib/$ABI/"
cp "$dex" "$stage/classes.dex"
cp -R "$here/example/ui" "$stage/assets/ui"
"$tools/aapt2" link -o "$(win "$out/app-base.apk")" --manifest "$(win "$here/example/android/AndroidManifest.xml")" -I "$(win "$platform/android.jar")"
python "$(win "$here/signApk.py")" pack "$(win "$out/app-base.apk")" "$(win "$stage")" "$(win "$out/app-packed.apk")"
"$tools/zipalign" -f -P 16 4 "$(win "$out/app-packed.apk")" "$(win "$out/app-aligned.apk")"
python "$(win "$here/signApk.py")" sign "$(win "$out/app-aligned.apk")" "$(win "$out/artful-cascade.apk")"
echo "built $out/artful-cascade.apk"
