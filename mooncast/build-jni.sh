#!/bin/sh
# Builds libchiaki-jni.so (with liboboe.so and libc++_shared.so) and copies them into a MoonCast app checkout.
# Usage: mooncast/build-jni.sh /path/to/moonlight-android
set -e
APP=${1:?usage: $0 /path/to/moonlight-android}
APP=$(cd "$APP" && pwd)
cd "$(dirname "$0")/../android"
./gradlew assembleDebug
OUT=app/build/intermediates/stripped_native_libs/debug/stripDebugDebugSymbols/out/lib
for abi in arm64-v8a armeabi-v7a; do
    mkdir -p "$APP/app/src/main/jniLibs/$abi"
    for lib in libchiaki-jni.so liboboe.so libc++_shared.so; do
        cp "$OUT/$abi/$lib" "$APP/app/src/main/jniLibs/$abi/"
    done
done
echo "Copied the libraries into $APP/app/src/main/jniLibs"
