#!/bin/sh
# End-to-end test of the SDK path (HantekWrapper.dll <-> named pipe <-> HantekProxy.exe)
# with a mock HTMarch.dll, run under Wine.
#
# Needs: g++-mingw-w64 (x86_64) and wine64. On Ubuntu: apt install g++-mingw-w64 wine64
#
# The real HantekProxy.exe is 32-bit (HTMarch.dll is 32-bit). Here the proxy and
# the mock are built as 64-bit so that everything runs in one 64-bit Wine; the
# source and the pipe protocol are the same (unsigned long is 32-bit on both).
set -e
cd "$(dirname "$0")"
ROOT=../..
OUT=${OUT:-$(mktemp -d)}
CC64=${CC64:-x86_64-w64-mingw32-gcc}
CXX64=${CXX64:-x86_64-w64-mingw32-g++}
WINE=${WINE:-$(command -v wine64 || command -v wine || echo /usr/lib/wine/wine64)}

"$CC64" -O2 -Wall -shared -o "$OUT/HTMarch.dll" mock_htmarch.c mock_htmarch.def
"$CXX64" -O2 -Wall -static -o "$OUT/HantekProxy.exe" "$ROOT/HantekProxy.cpp"
"$CXX64" -O2 -Wall -static -shared -o "$OUT/HantekWrapper.dll" "$ROOT/HantekWrapper.cpp" "$ROOT/HantekWrapper.def"
"$CXX64" -O2 -Wall -static -o "$OUT/test_sdk.exe" test_sdk.cpp "$OUT/HantekWrapper.dll"

cd "$OUT"
export WINEDEBUG=-all
"$WINE" test_sdk.exe
