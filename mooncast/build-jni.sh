#!/bin/sh
# Builds libchiaki-jni.so (with liboboe.so and libc++_shared.so) and copies them into a MoonCast app checkout.
# Usage: mooncast/build-jni.sh /path/to/moonlight-android
# The MoonCast app runs this itself (its buildChiaki task) when the libraries are missing or this tree changed.
set -e
APP=${1:?usage: $0 /path/to/moonlight-android}
APP=$(cd "$APP" && pwd)
cd "$(dirname "$0")/../android"

# The Android SDK: from the environment, else from the app's local.properties
SDK=${ANDROID_HOME:-$ANDROID_SDK_ROOT}
if [ -z "$SDK" ] && [ -f "$APP/local.properties" ]; then
    SDK=$(sed -n 's/^sdk\.dir=//p' "$APP/local.properties")
fi
[ -n "$SDK" ] || { echo "build-jni.sh: set ANDROID_HOME (or sdk.dir in the app's local.properties)" >&2; exit 1; }
echo "sdk.dir=$SDK" > local.properties

# nanopb generates chiaki's protocol code with protoc
command -v protoc >/dev/null 2>&1 || { echo "build-jni.sh: protoc is needed on the PATH (nanopb)" >&2; exit 1; }

./gradlew assembleDebug
OUT=app/build/intermediates/stripped_native_libs/debug/stripDebugDebugSymbols/out/lib
for abi in arm64-v8a armeabi-v7a; do
    mkdir -p "$APP/app/src/main/jniLibs/$abi"
    for lib in libchiaki-jni.so liboboe.so libc++_shared.so; do
        cp "$OUT/$abi/$lib" "$APP/app/src/main/jniLibs/$abi/"
    done
done
echo "Copied the libraries into $APP/app/src/main/jniLibs"
