#!/bin/bash -u
# We use set -e and bash with -u to bail on first non zero exit code of any
# processes launched or upon any unbound variable.
# We use set -x to print commands before running them to help with
# debugging.
set -ex

echo "START INSIDE CONTAINER - CORE"

echo "-- BUILD CORES:       $3"
echo "-- GITHUB_REPOSITORY: $1"
echo "-- GITHUB_SHA:        $2"
echo "-- GITHUB_RUN_NUMBER: $4"

umask 0000;

cd /io/ &&
echo "Importing env... Lines:" &&
cat .env|wc -l &&
source .env

echo $?
if [[ "$?" -ne "0" ]]; then
  echo "ERR no .env found/sourced"
  exit 127
fi

BUILD_TYPE=Release

mv Builds/CMake/deps/WasmEdge.cmake Builds/CMake/deps/WasmEdge.old &&
echo "find_package(LLVM REQUIRED CONFIG)
message(STATUS \"Found LLVM \${LLVM_PACKAGE_VERSION}\")
message(STATUS \"Using LLVMConfig.cmake in: \${LLVM_DIR}\")
add_library (wasmedge STATIC IMPORTED GLOBAL)
set_target_properties(wasmedge PROPERTIES IMPORTED_LOCATION \${WasmEdge_LIB})
target_link_libraries (ripple_libs INTERFACE wasmedge)
add_library (wasmedge::wasmedge ALIAS wasmedge)
message(\"WasmEdge DONE\")
" > Builds/CMake/deps/WasmEdge.cmake &&

export LDFLAGS="-static-libstdc++"

git config --global --add safe.directory /io &&
git checkout src/ripple/protocol/impl/BuildInfo.cpp &&
sed -i s/\"0.0.0\"/\"$(date +%Y).$(date +%-m).$(date +%-d)-$(git rev-parse --abbrev-ref HEAD)$(if [ -n "$4" ]; then echo "+$4"; fi)\"/g src/ripple/protocol/impl/BuildInfo.cpp &&
conan export external/snappy snappy/1.1.10@xahaud/stable &&
conan export external/soci soci/4.0.3@xahaud/stable &&
cd release-build &&
conan install .. --output-folder . --build missing --settings build_type=$BUILD_TYPE &&
cmake .. -G Ninja \
  -DCMAKE_BUILD_TYPE=$BUILD_TYPE \
  -DCMAKE_TOOLCHAIN_FILE:FILEPATH=build/generators/conan_toolchain.cmake \
  -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++" \
  -DLLVM_DIR=$LLVM_DIR \
  -DWasmEdge_LIB=$WasmEdge_LIB \
  -Dxrpld=TRUE \
  -Dtests=TRUE &&
ccache -z &&
ninja -j $3 &&
ccache -s &&
strip -s rippled &&	
mv rippled xahaud &&
libcheck xahaud &&
echo "Build host: `hostname`" > release.info &&
echo "Build date: `date`" >> release.info &&
echo "Build md5: `md5sum xahaud`" >> release.info &&
echo "Git remotes:" >> release.info && 
git remote -v >> release.info 
echo "Git status:" >> release.info &&
git status -v >> release.info &&
echo "Git log [last 20]:" >> release.info &&
git log -n 20 >> release.info;

if [[ "$4" == "" ]]; then
  # Non GH, local building
  echo "Non GH, local building, no Action runner magic"
else
  # GH Action, runner
  cp /io/release-build/xahaud /data/builds/$(date +%Y).$(date +%-m).$(date +%-d)-$(git rev-parse --abbrev-ref HEAD)+$4
  cp /io/release-build/release.info /data/builds/$(date +%Y).$(date +%-m).$(date +%-d)-$(git rev-parse --abbrev-ref HEAD)+$4.releaseinfo
  echo "Published build to: http://build.xahau.tech/"
  echo $(date +%Y).$(date +%-m).$(date +%-d)-$(git rev-parse --abbrev-ref HEAD)+$4
fi

cd ..;

mv src/ripple/net/impl/RegisterSSLCerts.cpp.old src/ripple/net/impl/RegisterSSLCerts.cpp;
mv Builds/CMake/deps/WasmEdge.old Builds/CMake/deps/WasmEdge.cmake;
rm src/certs/certbundle.h;
git checkout src/ripple/protocol/impl/BuildInfo.cpp;

echo "END INSIDE CONTAINER - CORE"
