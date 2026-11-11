#!/bin/bash
# Downloads OpenCASCADE and builds the STEP-related toolkits as static libraries
# into third_party/occt. Needed once, for STEP support. Requires git and cmake.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p third_party
cd third_party

[ -d occt-src ] || git clone --depth 1 --branch V7_9_1 https://github.com/Open-Cascade-SAS/OCCT.git occt-src

cmake -S occt-src -B occt-build \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_LIBRARY_TYPE=Static \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DBUILD_MODULE_ApplicationFramework=OFF -DBUILD_MODULE_DataExchange=OFF -DBUILD_MODULE_Draw=OFF \
    -DBUILD_MODULE_FoundationClasses=OFF -DBUILD_MODULE_ModelingAlgorithms=OFF -DBUILD_MODULE_ModelingData=OFF \
    -DBUILD_MODULE_Visualization=OFF -DBUILD_MODULE_DETools=OFF \
    "-DBUILD_ADDITIONAL_TOOLKITS=TKDESTEP;TKMesh" \
    -DUSE_FREETYPE=OFF -DUSE_TK=OFF -DUSE_TCL=OFF -DUSE_XLIB=OFF -DBUILD_DOC_Overview=OFF \
    "-DINSTALL_DIR=$PWD/occt"
cmake --build occt-build -j "$(sysctl -n hw.ncpu)"
cmake --install occt-build
echo "OpenCASCADE installed in third_party/occt"
