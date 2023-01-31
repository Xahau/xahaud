#!/bin/bash

echo "START INSIDE CONTAINER - CORE"

echo "-- BUILD CORES:       $3"
echo "-- GITHUB_REPOSITORY: $1"
echo "-- GITHUB_SHA:        $2"
echo "-- GITHUB_RUN_NUMBER: $4"

umask 0000;

export CPLUS_INCLUDE_PATH="/hbb_exe/include"
export MANPATH="/opt/rh/devtoolset-10/root/usr/share/man:"
export LDFLAGS="-L/hbb_exe/lib -static-libstdc++"
export HOSTNAME="vanity"
export CPPFLAGS="-I/hbb_exe/include"
export LIBRARY_PATH="/hbb_exe/lib"
export LDPATHFLAGS="-L/hbb_exe/lib"
export STATICLIB_CFLAGS="-g -O2 -fvisibility=hidden -I/hbb_exe/include "
export SHLIB_CXXFLAGS="-g -O2 -fvisibility=hidden -I/hbb_exe/include "
export PCP_DIR="/opt/rh/devtoolset-10/root"
export LD_LIBRARY_PATH="/hbb/lib:/hbb_exe/lib"
export O3_ALLOWED="true"
export CXXFLAGS="-g -O2 -fvisibility=hidden -I/hbb_exe/include "
export PATH="/hbb_exe/bin:/hbb/bin:/opt/rh/devtoolset-10/root/usr/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
export C_INCLUDE_PATH="/hbb_exe/include"
export STATICLIB_CXXFLAGS="-g -O2 -fvisibility=hidden -I/hbb_exe/include "
export SHLIB_LDFLAGS="-L/hbb_exe/lib -static-libstdc++"
export BOOST_INCLUDEDIR="/usr/local/src/boost_1_75_0"
export SHLVL="1"
export SHLIB_CFLAGS="-g -O2 -fvisibility=hidden -I/hbb_exe/include "
export CFLAGS="-g -O2 -fvisibility=hidden -I/hbb_exe/include "
export BOOST_ROOT="/usr/local/src/boost_1_75_0"
export PKG_CONFIG_PATH="/hbb_exe/lib/pkgconfig:/usr/lib/pkgconfig"
export INFOPATH="/opt/rh/devtoolset-10/root/usr/share/info"
export Boost_LIBRARY_DIRS="/usr/local/lib"

cd /io/ &&
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
cmake .. -DBoost_NO_BOOST_CMAKE=ON -DLLVM_DIR=/usr/lib64/llvm13/lib/cmake/llvm/ -DLLVM_LIBRARY_DIR=/usr/lib64/llvm13/lib/ -DWasmEdge_LIB=/usr/local/lib64/libwasmedge.a &&
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
cd ..;
mv src/ripple/net/impl/RegisterSSLCerts.cpp.old src/ripple/net/impl/RegisterSSLCerts.cpp;
mv Builds/CMake/deps/Rocksdb.cmake.old Builds/CMake/deps/Rocksdb.cmake;
mv Builds/CMake/deps/WasmEdge.old Builds/CMake/deps/WasmEdge.cmake;

echo "END INSIDE CONTAINER - CORE"
