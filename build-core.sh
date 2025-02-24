#!/bin/bash

set -e

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

perl -i -pe "s/^(\\s*)-DBUILD_SHARED_LIBS=OFF/\\1-DBUILD_SHARED_LIBS=OFF\\n\\1-DROCKSDB_BUILD_SHARED=OFF/g" Builds/CMake/deps/Rocksdb.cmake &&
mv Builds/CMake/deps/WasmEdge.cmake Builds/CMake/deps/WasmEdge.old &&
echo "find_package(LLVM REQUIRED CONFIG)
message(STATUS \"Found LLVM ${LLVM_PACKAGE_VERSION}\")
message(STATUS \"Using LLVMConfig.cmake in: \${LLVM_DIR}\")
add_library (wasmedge STATIC IMPORTED GLOBAL)
set_target_properties(wasmedge PROPERTIES IMPORTED_LOCATION \${WasmEdge_LIB})
target_link_libraries (ripple_libs INTERFACE wasmedge)
add_library (NIH::WasmEdge ALIAS wasmedge)
message(\"WasmEdge DONE\")
" > Builds/CMake/deps/WasmEdge.cmake &&
git checkout src/ripple/protocol/impl/BuildInfo.cpp &&
sed -i s/\"0.0.0\"/\"$(date +%Y).$(date +%-m).$(date +%-d)-$(git rev-parse --abbrev-ref HEAD)+$4\"/g src/ripple/protocol/impl/BuildInfo.cpp &&
cd release-build &&
cmake .. -DCMAKE_BUILD_TYPE=Release -DBoost_NO_BOOST_CMAKE=ON -DLLVM_DIR=/usr/lib64/llvm13/lib/cmake/llvm/ -DLLVM_LIBRARY_DIR=/usr/lib64/llvm13/lib/ -DWasmEdge_LIB=/usr/local/lib64/libwasmedge.a &&
make -j$3 VERBOSE=1 &&
strip -s rippled &&
mv rippled xahaud &&
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
mv Builds/CMake/deps/Rocksdb.cmake.old Builds/CMake/deps/Rocksdb.cmake;
mv Builds/CMake/deps/WasmEdge.old Builds/CMake/deps/WasmEdge.cmake;


echo "END INSIDE CONTAINER - CORE"
