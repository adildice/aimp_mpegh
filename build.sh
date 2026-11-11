#!/bin/sh
# Cross-builds aimp_mpegh.dll for Windows with mingw-w64: 64-bit (out/x64) and 32-bit (out/x86).
# Expects:  $DEPS/build    - mpeghdec built with mingw.cmake   (64-bit static libs)
#           $DEPS/build32  - mpeghdec built with mingw32.cmake (32-bit static libs, optional)
#           $DEPS/mpeghdec - mpeghdec source checkout
#           $SDK           - AIMP SDK C++ headers (apiPlugin.h etc.)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
DEPS=${DEPS:-$HERE/deps}
SDK=${SDK:-$HERE/aimp_sdk}
build() { # $1 = compiler triplet, $2 = deps build dir, $3 = output subdir
  B=$DEPS/$2
  [ -d "$B/lib" ] || { echo "skipping $3: $B not built"; return 0; }
  CXX=$1-g++-posix
  INC="-I$SDK -I$DEPS/mpeghdec/include -I$B/_deps/mmtisobmff-src/include -I$B/_deps/ilo-src/include"
  mkdir -p "$HERE/out/$3"
  $CXX -std=c++14 -O2 -Wall -Wno-unknown-pragmas -Wno-attributes -shared -static \
    -DMPEGHDEC_STATIC $INC "$HERE/src/aimp_mpegh.cpp" \
    -L$B/lib -lmpeghdec -lmmtisobmff -lilo -luuid \
    -Wl,--kill-at -s -o "$HERE/out/$3/aimp_mpegh.dll"
  $CXX -std=c++14 -O1 -static -Wno-attributes $INC "$HERE/test/test_host.cpp" -luuid -o "$HERE/out/$3/test_host.exe"
}
build x86_64-w64-mingw32 build x64
build i686-w64-mingw32 build32 x86
ls -la "$HERE/out/x64" "$HERE/out/x86" 2>/dev/null
