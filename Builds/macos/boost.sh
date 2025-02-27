#!/bin/bash -u
# We use set -e and bash with -u to bail on first non zero exit code of any
# processes launched or upon any unbound variable.
# We use set -x to print commands before running them to help with
# debugging.
set -ex

source "$(dirname "$0")/env.sh"
cd $XAHAUD_DEPS

DOWNLOAD="$BOOST_FOLDER_NAME.tar.gz"

if [[ ! -f "$DOWNLOAD" ]]; then
  wget https://archives.boost.io/release//$BOOST_VERSION/source/$DOWNLOAD
fi
if [[ ! -d "$BOOST_FOLDER_NAME" ]]; then
  tar -xzf $DOWNLOAD
fi

cd $BOOST_FOLDER_NAME
./bootstrap.sh
./b2 -j$(sysctl -n hw.logicalcpu)

