#!/bin/bash -u
# We use set -e and bash with -u to bail on first non zero exit code of any
# processes launched or upon any unbound variable.
# We use set -x to print commands before running them to help with
# debugging.
set -ex

source "$(dirname "$0")/env.sh"

# Assert that the required directories exist
assert_dir_exists() {
  if [[ ! -d "$1" ]]; then
    echo "ERROR: Directory $1 does not exist!" >&2
    exit 1
  fi
}

# Quick sanity check
assert_dir_exists "$BOOST_ROOT"
assert_dir_exists "$BOOST_LIBRARY_DIRS"
assert_dir_exists "$BOOST_INCLUDE_DIR"
assert_dir_exists "$LLVM_PREFIX"
assert_dir_exists "$PROTOBUF_FOLDER_NAME"
assert_dir_exists "$WASMEDGE_FOLDER_NAME"

mkdir -p build && cd build

cmake -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_DIR=$LLVM_DIR \
    -DLLVM_LIBRARY_DIR=$LLVM_LIBRARY_DIR \
    ..

cmake --build . \
      --target rippled \
      --parallel \
      -j$(sysctl -n hw.logicalcpu)
