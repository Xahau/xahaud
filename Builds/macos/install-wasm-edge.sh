#!/bin/bash -u
# We use set -e and bash with -u to bail on first non zero exit code of any
# processes launched or upon any unbound variable.
# We use set -x to print commands before running them to help with
# debugging.
set -ex

source "$(dirname "$0")/env.sh"
cd $XAHAUD_DEPS

DOWNLOAD="WasmEdge-$WASMEDGE_VERSION.zip"

if [[ ! -f "$DOWNLOAD" ]]; then
  wget -O $DOWNLOAD https://github.com/WasmEdge/WasmEdge/archive/refs/tags/$WASMEDGE_VERSION.zip
fi
if [[ ! -d "WasmEdge-$WASMEDGE_VERSION" ]]; then
  unzip -o $DOWNLOAD
fi

cd $WASMEDGE_FOLDER_NAME

mkdir -p build
cd build

cmake -DCMAKE_BUILD_TYPE=Release \
  -DWASMEDGE_BUILD_SHARED_LIB=OFF \
  -DWASMEDGE_BUILD_STATIC_LIB=ON \
  -DWASMEDGE_BUILD_AOT_RUNTIME=ON \
  -DWASMEDGE_FORCE_DISABLE_LTO=ON \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DWASMEDGE_LINK_LLVM_STATIC=ON \
  -DWASMEDGE_BUILD_PLUGINS=OFF \
  -DWASMEDGE_LINK_TOOLS_STATIC=ON \
  ..

make -j$(sysctl -n hw.logicalcpu)

sudo make install

