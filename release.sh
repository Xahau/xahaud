#!/bin/bash

# Define the build identifier and git abbreviation
GIT_ABBRV=$(git rev-parse --abbrev-ref HEAD)
GIT_SHA=$1  # Get the SHA from the argument

# Create a temporary container
container_id=$(docker create transia/xahaud-binary:$GIT_SHA)

# Copy the xahaud and release.info files from the container to the host
docker cp $container_id:/io/release-build/xahaud /data/builds/$(date +%Y).$(date +%-m).$(date +%-d)-$GIT_ABBRV+$GITHUB_RUN_NUMBER
docker cp $container_id:/io/release-build/release.info /data/builds/$(date +%Y).$(date +%-m).$(date +%-d)-$GIT_ABBRV+$GITHUB_RUN_NUMBER.releaseinfo

# Remove the temporary container
docker rm $container_id

# Print the published build
echo "Published build to: http://build.xahau.tech/"
echo $(date +%Y).$(date +%-m).$(date +%-d)-$GIT_ABBRV+$GITHUB_RUN_NUMBER

# Restore the original files
# mv src/ripple/net/impl/RegisterSSLCerts.cpp.old src/ripple/net/impl/RegisterSSLCerts.cpp
# mv Builds/CMake/deps/Rocksdb.cmake.old Builds/CMake/deps/Rocksdb.cmake
# mv Builds/CMake/deps/WasmEdge.old Builds/CMake/deps/WasmEdge.cmake