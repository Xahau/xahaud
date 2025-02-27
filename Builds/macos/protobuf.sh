#!/bin/bash -u
# We use set -e and bash with -u to bail on first non zero exit code of any
# processes launched or upon any unbound variable.
# We use set -x to print commands before running them to help with
# debugging.
set -ex

source "$(dirname "$0")/env.sh"
cd "$XAHAUD_DEPS"

PROTOBUF_DOWNLOAD="protobuf-all-$PROTOBUF_VERSION.tar.gz"

if [[ ! -f "$PROTOBUF_DOWNLOAD" ]]; then
  wget https://github.com/protocolbuffers/protobuf/releases/download/v$PROTOBUF_VERSION/$PROTOBUF_DOWNLOAD
fi
if [[ ! -d "$PROTOBUF_FOLDER_NAME" ]]; then
  tar -xzf $PROTOBUF_DOWNLOAD
fi

cd $PROTOBUF_FOLDER_NAME
./autogen.sh
./configure --prefix=/usr/local --disable-shared link=static
make -j$(sysctl -n hw.logicalcpu)

sudo make install
