#!/bin/sh
# Builds HantekUSB.dll (64-bit Windows, loaded by MATLAB) with libusb linked in
# statically, so the DLL needs nothing but Windows itself.
#
#   ./build_libusb.sh          Windows DLL (needs x86_64-w64-mingw32-gcc: on Ubuntu
#                              apt install gcc-mingw-w64-x86-64; in MSYS2 use the
#                              MINGW64 shell with CC64=gcc)
#   ./build_libusb.sh linux    libHantekUSB.so for the tests (needs libusb-1.0-0-dev)
#
# libusb is built once from third_party/libusb-1.0.29.tar.bz2 into .build/.
set -e
cd "$(dirname "$0")"

if [ "$1" = linux ]; then
    ${CC:-gcc} -O2 -Wall -Wextra -fPIC -shared -o "${OUT:-.}/libHantekUSB.so" HantekUSB.c \
        $(pkg-config --cflags --libs libusb-1.0) -lpthread -lm
    exit 0
fi

CC64=${CC64:-x86_64-w64-mingw32-gcc}
BUILD=$(pwd)/.build
LU=$BUILD/libusb-win64
if [ ! -f "$LU/lib/libusb-1.0.a" ]; then
    mkdir -p "$BUILD"
    tar -xjf third_party/libusb-1.0.29.tar.bz2 -C "$BUILD"
    echo "Building libusb (log: .build/libusb-build.log)"
    if ! (cd "$BUILD/libusb-1.0.29" &&
            ./configure --host=x86_64-w64-mingw32 CC="$CC64" --prefix="$LU" \
                --enable-static --disable-shared &&
            make -j4 && make install) > "$BUILD/libusb-build.log" 2>&1; then
        echo "libusb build failed, see .build/libusb-build.log"
        exit 1
    fi
fi
"$CC64" -O2 -s -Wall -Wextra -shared -static-libgcc -o HantekUSB.dll HantekUSB.c \
    -I"$LU/include/libusb-1.0" "$LU/lib/libusb-1.0.a"
echo "Built HantekUSB.dll"
