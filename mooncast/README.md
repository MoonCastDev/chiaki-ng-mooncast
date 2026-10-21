# chiaki-ng for MoonCast

This is [chiaki-ng](https://github.com/streetpea/chiaki-ng) (AGPL-3.0) with the changes MoonCast needs for its native PS5 Remote Play.
MoonCast's Android apps ([MoonCastDev/moonlight-android](https://github.com/MoonCastDev/moonlight-android)) ship the
`libchiaki-jni.so` built from this tree, together with Oboe's `liboboe.so` and the NDK's `libc++_shared.so`.

- Base: upstream `main` at `c8bea264` (2026-10-09), branch `upstream` here.
- MoonCast's changes: branch `mooncast`, one commit on top of the base (`git diff upstream mooncast`).

## What MoonCast changes

- `android/app/src/main/cpp/chiaki-jni.c`: Remote Play over the internet through PSN (hole punching with port guessing, no VPN
  or port forwarding); real packet-loss reporting (up to 5%) and a fresh keyframe when a frame can't be repaired, so the PS5
  lowers its bitrate on a lossy link; stream stats for the app's overlay.
- `android/app/src/main/cpp/video-decoder.c/.h`: a clean decoder shutdown (no codec call after stop, which aborts on
  Android 9; no input once shutting down or when the codec has lost its surface), MoonCast issue #37.
- `lib/src/remote/holepunch.c`, `lib/src/aia_openssl.c`: Android's curl has no CA store, so the app hands it the device's
  trusted roots (`MC_CA_BUNDLE`) for PSN; a longer wait (about 60 s) for a console in rest mode to join the PSN session.
- `lib/src/streamconnection.c`, `lib/include/chiaki/streamconnection.h`: the round-trip time from the latest
  connection-quality message, for the stats.
- `android/app/build.gradle`: only the two ABIs MoonCast ships (arm64-v8a, armeabi-v7a). `android/gradle.properties`: build memory.

`patch-scripts/` keeps the scripts the changes were first made with, for the record; the tree already has them applied.

## Building the libraries

Needs JDK 17, the Android SDK with NDK 28.2.13676358 and CMake 3.22.1, and Python 3 with `protobuf` (for nanopb).

```sh
git clone --recurse-submodules -b mooncast https://github.com/MoonCastDev/chiaki-ng-mooncast
cd chiaki-ng-mooncast
mooncast/build-jni.sh /path/to/moonlight-android
```

The script builds chiaki-ng's Android app and copies the stripped libraries for both ABIs into
`app/src/main/jniLibs/` of the MoonCast app checkout. The SDK comes from `ANDROID_HOME`, or from the app's `local.properties`.

The MoonCast app has this repo as a submodule (`third-party/chiaki-ng`) and runs the script itself during its build
when the libraries are missing or this tree changed, so you normally don't run it by hand.
