#!/bin/bash -u
# We use set -e and bash with -u to bail on first non zero exit code of any
# processes launched or upon any unbound variable.
# We use set -x to print commands before running them to help with
# debugging.
set -ex

source "$(dirname "$0")/env.sh"

brew install automake \
             wget \
             curl \
             git \
             pkg-config \
             openssl \
             autoconf \
             libtool \
             unzip \
             cmake \
             "llvm@$LLVM_VERSION"
