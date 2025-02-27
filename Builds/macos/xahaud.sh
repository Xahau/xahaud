#!/bin/bash -u
# We use set -e and bash with -u to bail on first non zero exit code of any
# processes launched or upon any unbound variable.
# We use set -x to print commands before running them to help with
# debugging.
set -ex

source "$(dirname "$0")/env.sh"

# Quick sanity check
ls $BOOST_ROOT
ls $BOOST_LIBRARY_DIRS
ls $BOOST_INCLUDE_DIR
ls $LLVM_PREFIX
ls $PROTOBUF_FOLDER_NAME
ls $WASMEDGE_FOLDER_NAME

mkdir -p build && cd build

cmake -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_DIR=$LLVM_DIR \
    -DLLVM_LIBRARY_DIR=$LLVM_LIBRARY_DIR \
    ..

cmake --build . \
      --target rippled \
      --parallel \
      -j$(sysctl -n hw.logicalcpu)
