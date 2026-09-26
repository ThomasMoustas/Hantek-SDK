#!/bin/sh
# Tests of libusb/HantekUSB without a scope:
#   1. test_hantekusb.c against libHantekUSB.so built from libusb/HantekUSB.c (Linux)
#   2. the same test against the committed libusb/HantekUSB.dll under Wine
#   3. test_usb_fake.c: HantekUSB's USB code against fake_libusb.c, a model of
#      the 6022BL with the sigrok firmware (upload, re-enumeration, streaming)
# Needs: gcc, libusb-1.0-0-dev, gcc-mingw-w64-x86-64, wine64.
set -e
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
OUT=${OUT:-$(mktemp -d)}
WINE=${WINE:-$(command -v wine64 || command -v wine || echo /usr/lib/wine/wine64)}

echo "== Linux (libHantekUSB.so)"
mkdir -p "$OUT/linux"
OUT="$OUT/linux" "$ROOT/libusb/build_libusb.sh" linux
cp "$ROOT"/libusb/*.fw "$OUT/linux/"
${CC:-gcc} -O2 -Wall -o "$OUT/linux/test_hantekusb" test_hantekusb.c \
    -L"$OUT/linux" -lHantekUSB -Wl,-rpath,'$ORIGIN'
(cd "$OUT/linux" && ./test_hantekusb .)

echo "== Windows (libusb/HantekUSB.dll under Wine)"
mkdir -p "$OUT/win"
cp "$ROOT/libusb/HantekUSB.dll" "$ROOT"/libusb/*.fw "$OUT/win/"
${CC64:-x86_64-w64-mingw32-gcc} -O2 -Wall -o "$OUT/win/test_hantekusb.exe" test_hantekusb.c \
    "$OUT/win/HantekUSB.dll"
(cd "$OUT/win" && WINEDEBUG=-all "$WINE" test_hantekusb.exe .)

echo "== Linux, HantekUSB.c against the modelled scope (fake_libusb.c)"
mkdir -p "$OUT/fake"
${CC:-gcc} -O2 -Wall -Wextra -o "$OUT/fake/test_usb_fake" test_usb_fake.c fake_libusb.c \
    "$ROOT/libusb/HantekUSB.c" $(pkg-config --cflags libusb-1.0) -lpthread -lm
"$OUT/fake/test_usb_fake" "$ROOT/libusb"
