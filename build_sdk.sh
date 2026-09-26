#!/bin/sh
# Builds the SDK path:
#   HantekProxy.exe   32-bit, loads the 32-bit Hantek SDK (HTMarch.dll)
#   HantekWrapper.dll 64-bit, loaded by MATLAB, talks to the proxy over a named pipe
#
# Linux / WSL:  sudo apt install g++-mingw-w64   then   ./build_sdk.sh
# MSYS2:        pacman -S mingw-w64-i686-gcc mingw-w64-x86_64-gcc, put /mingw32/bin
#               and /mingw64/bin on PATH, then ./build_sdk.sh
# Other compiler names: CXX32=... CXX64=... ./build_sdk.sh
set -e
cd "$(dirname "$0")"
CXX32=${CXX32:-i686-w64-mingw32-g++}
CXX64=${CXX64:-x86_64-w64-mingw32-g++}

"$CXX32" -O2 -s -static -Wall -o HantekProxy.exe HantekProxy.cpp
"$CXX64" -O2 -s -static -shared -Wall -o HantekWrapper.dll HantekWrapper.cpp HantekWrapper.def
echo "Built HantekProxy.exe and HantekWrapper.dll"
