#!/bin/bash -x

BUILD_CORES=$(echo "scale=0 ; `nproc` / 1.337" | bc)

if [[ "$GITHUB_REPOSITORY" == "" ]]; then
  #Default
  BUILD_CORES=8
fi

echo "Mounting $(pwd)/io in ubuntu and running unit tests"
docker run --rm -i -v $(pwd):/io -e BUILD_CORES=$BUILD_CORES ubuntu sh -c '/io/release-build/xahaud --unittest-jobs $BUILD_CORES -u'
