#!/bin/bash
# MoonCast for iPad: builds libchiaki and its dependencies for iPadOS (arm64) into one static library
# usage: mooncast/build-ios.sh <mooncast-ios checkout>   (after one Xcode build, so the OpenSSL package is downloaded)
# needs: Homebrew cmake ninja protobuf; a python with the protobuf module (PY=...)
set -e
APP=${1:?mooncast-ios path}
PY=${PY:-python3}
DD=${DD:-$HOME/mooncast/dd}
DEPS=${DEPS:-$HOME/mooncast/ios-deps}
export PATH=/opt/homebrew/bin:$PATH
FW=$DD/SourcePackages/artifacts/openssl-package/OpenSSL/OpenSSL.xcframework/ios-arm64/OpenSSL.framework
IOS="-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 -DCMAKE_POLICY_VERSION_MINIMUM=3.5"

# libevent (holepunch uses it off Android)
mkdir -p $DEPS && cd $DEPS
[ -d libevent-2.1.12-stable ] || curl -sL https://github.com/libevent/libevent/releases/download/release-2.1.12-stable/libevent-2.1.12-stable.tar.gz | tar xz
cmake -S libevent-2.1.12-stable -B libevent-2.1.12-stable/build-ios -G Ninja $IOS -DEVENT__LIBRARY_TYPE=STATIC -DEVENT__DISABLE_OPENSSL=ON \
  -DEVENT__DISABLE_MBEDTLS=ON -DEVENT__DISABLE_TESTS=ON -DEVENT__DISABLE_SAMPLES=ON -DEVENT__DISABLE_BENCHMARK=ON -DEVENT__DISABLE_REGRESS=ON \
  -DCMAKE_INSTALL_PREFIX=$DEPS/prefix
cmake --build libevent-2.1.12-stable/build-ios && cmake --install libevent-2.1.12-stable/build-ios

# OpenSSL headers laid out as include/openssl for FindOpenSSL
mkdir -p $DEPS/openssl/include && ln -sfn $FW/Headers $DEPS/openssl/include/openssl

cd "$(dirname "$0")/.."
cmake -S . -B build-ios -G Ninja $IOS -DCMAKE_BUILD_TYPE=Release \
  -DCHIAKI_ENABLE_TESTS=OFF -DCHIAKI_ENABLE_CLI=OFF -DCHIAKI_ENABLE_GUI=OFF -DCHIAKI_ENABLE_SETSU=OFF -DCHIAKI_ENABLE_FFMPEG_DECODER=OFF \
  -DCHIAKI_ENABLE_PI_DECODER=OFF -DCHIAKI_ENABLE_SPEEX=OFF -DCHIAKI_ENABLE_STEAM_SHORTCUT=OFF -DCHIAKI_ENABLE_STEAMDECK_NATIVE=OFF \
  -DCHIAKI_LIB_ENABLE_OPUS=ON -DOpus_INCLUDE_DIRS=$APP/libs/opus/include -DOpus_LIBRARIES=$APP/libs/opus/lib/iOS/libopus.a \
  -DCHIAKI_LIB_JSONC_EXTERNAL_PROJECT=ON -DCHIAKI_LIB_MINIUPNPC_EXTERNAL_PROJECT=ON \
  -DOPENSSL_ROOT_DIR=$DEPS/openssl -DOPENSSL_INCLUDE_DIR=$DEPS/openssl/include -DOPENSSL_CRYPTO_LIBRARY=$FW/OpenSSL -DOPENSSL_SSL_LIBRARY=$FW/OpenSSL \
  -DLIBEVENT_INCLUDE_DIRS=$DEPS/prefix/include -DLIBEVENT_LIBRARIES=$DEPS/prefix/lib/libevent.a \
  -DCURL_USE_LIBSSH2=OFF -DCURL_USE_LIBSSH=OFF -DUSE_NGHTTP2=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DUSE_LIBIDN2=OFF -DCURL_USE_LIBPSL=OFF \
  -DCURL_DISABLE_LDAP=ON -DCURL_USE_GSSAPI=OFF \
  -DPYTHON_EXECUTABLE=$PY -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH
cmake --build build-ios --target chiaki-lib

B=build-ios
mkdir -p $APP/chiaki/lib/iOS $APP/chiaki/include
libtool -static -o $APP/chiaki/lib/iOS/libchiaki-all.a $B/lib/libchiaki.a $B/third-party/curl/lib/libcurl.a $B/third-party/libgf_complete.a \
  $B/third-party/libjerasure.a $B/third-party/nanopb/libprotobuf-nanopb.a $B/_deps/json-c-build/libjson-c.a $B/_deps/miniupnpc-build/libminiupnpc.a \
  $DEPS/prefix/lib/libevent.a
rm -rf $APP/chiaki/include/chiaki && cp -R lib/include/chiaki $APP/chiaki/include/ && cp $B/lib/include/chiaki/config.h $APP/chiaki/include/chiaki/
find $B/lib -name "*.pb.h" -exec cp {} $APP/chiaki/include/ \;
cp third-party/nanopb/pb*.h $APP/chiaki/include/
echo "libchiaki-all.a + headers -> $APP/chiaki"
