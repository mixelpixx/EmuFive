#!/bin/sh
# Builds jaguarTest and fmaStress against KytyPS5's jaguarCompat.cpp (build/kytyps5 with the
# patches applied and a PC build done, for Zydis and xbyak).
set -e
cd "$(dirname "$0")"
K=../../build/kytyps5
Z=$K/_Build/pc/_deps
for tool in jaguarTest fmaStress; do
  g++ -O1 -g -std=c++20 -ffp-contract=off -DXBYAK_NO_EXCEPTION -DZYDIS_STATIC_BUILD -DZYCORE_STATIC_BUILD \
    -I$K/src -I$Z/zydis-src/include -I$Z/zydis-src/dependencies/zycore/include \
    -I$Z/zydis-build -I$Z/zydis-build/zycore -I$Z/xbyak-src \
    $tool.cpp $K/src/loader/jaguarCompat.cpp \
    $Z/zydis-build/libZydis.a $Z/zydis-build/zycore/libZycore.a -o $tool
done
