#!/bin/bash -x

BUILD_CORES=$(echo "scale=0 ; `nproc` / 1.337" | bc)

if [[ "$GITHUB_REPOSITORY" == "" ]]; then
  #Default
  BUILD_CORES=8
fi

echo "Mounting $(pwd)/io in ubuntu and running unit tests"
./bin/fetch-jshookz-provider &&
docker run --rm -i -v $(pwd):/io --platform=linux/amd64 \
  -e BUILD_CORES=$BUILD_CORES \
  -e XAHAU_QJS_PROVIDER_WASM=/io/external/quickjs-provider/jshookz_provider.wasm \
  -e XAHAU_REQUIRE_QJS_PROVIDER_TESTS=1 \
  ubuntu sh -c '/io/release-build/xahaud --unittest-jobs $BUILD_CORES -u'
