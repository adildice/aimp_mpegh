#!/bin/sh
# Cross-builds aimp_mpegh.dll for 64-bit Windows with mingw-w64.
# Expects:  $DEPS/build   - mpeghdec built with the mingw toolchain (static libs)
#           $DEPS/mpeghdec - mpeghdec source checkout
#           $SDK           - AIMP SDK C++ headers (apiPlugin.h etc.)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
DEPS=${DEPS:-$HERE/deps}
SDK=${SDK:-$HERE/aimp_sdk}
B=$DEPS/build
CXX=x86_64-w64-mingw32-g++-posix
INC="-I$SDK -I$DEPS/mpeghdec/include -I$B/_deps/mmtisobmff-src/include -I$B/_deps/ilo-src/include"
mkdir -p "$HERE/out"
$CXX -std=c++14 -O2 -Wall -Wno-unknown-pragmas -Wno-attributes -shared -static \
  -DMPEGHDEC_STATIC \
  $INC "$HERE/src/aimp_mpegh.cpp" \
  -L$B/lib -lmpeghdec -lmmtisobmff -lilo -luuid \
  -Wl,--kill-at -s -o "$HERE/out/aimp_mpegh.dll"
$CXX -std=c++14 -O1 -static -Wno-attributes $INC "$HERE/test/test_host.cpp" -luuid -o "$HERE/out/test_host.exe"
ls -la "$HERE/out"
