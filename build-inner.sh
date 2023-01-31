#!/bin/bash

echo "START INSIDE CONTAINER"

umask 0000;
cd /io;
mkdir src/certs;
curl --silent -k https://raw.githubusercontent.com/RichardAH/rippled-release-builder/main/ca-bundle/certbundle.h -o src/certs/certbundle.h;
if [ "`grep certbundle.h src/ripple/net/impl/RegisterSSLCerts.cpp | wc -l`" -eq "0" ]
then
    cp src/ripple/net/impl/RegisterSSLCerts.cpp src/ripple/net/impl/RegisterSSLCerts.cpp.old
    perl -i -pe "s/^{/{
    #ifdef EMBEDDED_CA_BUNDLE
    BIO *cbio = BIO_new_mem_buf(ca_bundle.data(), ca_bundle.size());
    X509_STORE  *cts = SSL_CTX_get_cert_store(ctx.native_handle());
    if(!cts || !cbio)
        JLOG(j.warn())
            << \"Failed to create cts\/cbio when loading embedded certs.\";
    else
    {
        X509_INFO *itmp;
        int i, count = 0, type = X509_FILETYPE_PEM;
        STACK_OF(X509_INFO) *inf = PEM_X509_INFO_read_bio(cbio, NULL, NULL, NULL);

        if (!inf)
        {
            BIO_free(cbio);
            JLOG(j.warn())
                << \"Failed to read cbio when loading embedded certs.\";
        }
        else
        {
            for (i = 0; i < sk_X509_INFO_num(inf); i++)
            {
                itmp = sk_X509_INFO_value(inf, i);
                if (itmp->x509)
                {
                      X509_STORE_add_cert(cts, itmp->x509);
                      count++;
                }
                if (itmp->crl)
                {
                      X509_STORE_add_crl(cts, itmp->crl);
                      count++;
                }
            }
            sk_X509_INFO_pop_free(inf, X509_INFO_free);
            BIO_free(cbio);
        }
    }
    #endif/g" src/ripple/net/impl/RegisterSSLCerts.cpp &&
    sed -i "s/#include <ripple\/net\/RegisterSSLCerts.h>/\0\n#include <certs\/certbundle.h>/g" src/ripple/net/impl/RegisterSSLCerts.cpp
fi
rm -rf release-build;
mkdir release-build;
mkdir .nih_c;
mkdir .nih_toolchain;
cd .nih_toolchain &&
yum install -y wget lz4 lz4-devel git llvm13-static.x86_64 llvm13-devel.x86_64 devtoolset-10-binutils zlib-static ncurses-static -y \
  devtoolset-7-gcc-c++ \
  devtoolset-9-gcc-c++ \
  devtoolset-10-gcc-c++ \
  snappy snappy-devel \
  zlib zlib-devel \
  lz4-devel \
  libasan &&
export PATH=`echo $PATH | sed -E "s/devtoolset-9/devtoolset-7/g"` &&
echo "-- Install ZStd 1.1.3 --" &&
yum install epel-release -y &&
ZSTD_VERSION="1.1.3" &&
( wget -nc -q -O zstd-${ZSTD_VERSION}.tar.gz https://github.com/facebook/zstd/archive/v${ZSTD_VERSION}.tar.gz; echo "" ) &&
tar xzvf zstd-${ZSTD_VERSION}.tar.gz &&
cd zstd-${ZSTD_VERSION} &&
make -j8 install &&
cd .. &&
echo "-- Install Cmake 3.23.1 --" &&
pwd &&
( wget -nc -q https://github.com/Kitware/CMake/releases/download/v3.23.1/cmake-3.23.1-linux-x86_64.tar.gz; echo "" ) &&
tar -xzf cmake-3.23.1-linux-x86_64.tar.gz -C /hbb/ &&
echo "-- Install Boost 1.75.0 --" &&
pwd &&
( wget -nc -q https://boostorg.jfrog.io/artifactory/main/release/1.75.0/source/boost_1_75_0.tar.gz; echo "" ) &&
tar -xzf boost_1_75_0.tar.gz &&
cd boost_1_75_0 && ./bootstrap.sh && ./b2  link=static -j8 && ./b2 install &&
cd ../ &&
echo "-- Install Protobuf 3.20.0 --" &&
pwd &&
( wget -nc -q https://github.com/protocolbuffers/protobuf/releases/download/v3.20.0/protobuf-all-3.20.0.tar.gz; echo "" ) &&
tar -xzf protobuf-all-3.20.0.tar.gz &&
cd protobuf-3.20.0/ &&
./autogen.sh && ./configure --prefix=/usr --disable-shared link=static && make -j8 && make install &&
cd .. &&
echo "-- Build LLD --" &&
pwd &&
ln /usr/bin/llvm-config-13 /usr/bin/llvm-config &&
mv /opt/rh/devtoolset-9/root/usr/bin/ar /opt/rh/devtoolset-9/root/usr/bin/ar-9 &&
ln /opt/rh/devtoolset-10/root/usr/bin/ar  /opt/rh/devtoolset-9/root/usr/bin/ar &&
( wget -nc -q https://github.com/llvm/llvm-project/releases/download/llvmorg-13.0.1/lld-13.0.1.src.tar.xz; echo "" ) &&
( wget -nc -q https://github.com/llvm/llvm-project/releases/download/llvmorg-13.0.1/libunwind-13.0.1.src.tar.xz; echo "" ) &&
tar -xf lld-13.0.1.src.tar.xz &&
tar -xf libunwind-13.0.1.src.tar.xz &&
cp -r libunwind-13.0.1.src/include libunwind-13.0.1.src/src lld-13.0.1.src/ &&
cd lld-13.0.1.src &&
rm -rf build CMakeCache.txt &&
mkdir build &&
cd build &&
cmake .. -DLLVM_LIBRARY_DIR=/usr/lib64/llvm13/lib/ -DCMAKE_INSTALL_PREFIX=/usr/lib64/llvm13/ -DCMAKE_BUILD_TYPE=Release &&
make -j8 install &&
ln -s /usr/lib64/llvm13/lib/include/lld /usr/include/lld &&
cp /usr/lib64/llvm13/lib/liblld*.a /usr/local/lib/ &&
cd ../../ &&
echo "-- Build WasmEdge --" &&
( wget -nc -q https://github.com/WasmEdge/WasmEdge/archive/refs/tags/0.11.2.zip; unzip -o 0.11.2.zip; ) &&
cd WasmEdge-0.11.2 &&
( mkdir build; echo "" ) &&
cd build &&
export BOOST_ROOT="/usr/local/src/boost_1_75_0" &&
export Boost_LIBRARY_DIRS="/usr/local/lib" &&
export BOOST_INCLUDEDIR="/usr/local/src/boost_1_75_0" &&
export PATH=`echo $PATH | sed -E "s/devtoolset-7/devtoolset-9/g"` &&
cmake .. \
    -DWASMEDGE_BUILD_SHARED_LIB=OFF \
    -DWASMEDGE_BUILD_STATIC_LIB=ON \
    -DWASMEDGE_BUILD_AOT_RUNTIME=ON \
    -DWASMEDGE_FORCE_DISABLE_LTO=ON \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DWASMEDGE_LINK_LLVM_STATIC=ON \
    -DWASMEDGE_BUILD_PLUGINS=OFF \
    -DWASMEDGE_LINK_TOOLS_STATIC=ON \
    -DBoost_NO_BOOST_CMAKE=ON -DLLVM_DIR=/usr/lib64/llvm13/lib/cmake/llvm/ -DLLVM_LIBRARY_DIR=/usr/lib64/llvm13/lib/ &&
make -j8 install &&
export PATH=`echo $PATH | sed -E "s/devtoolset-9/devtoolset-10/g"` &&
cp -r include/api/wasmedge /usr/include/ &&
cd /io/ &&
echo "-- Build Rippled --" &&
pwd &&
cp Builds/CMake/deps/Rocksdb.cmake Builds/CMake/deps/Rocksdb.cmake.old &&
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
cd release-build &&
cmake .. -DBoost_NO_BOOST_CMAKE=ON -DLLVM_DIR=/usr/lib64/llvm13/lib/cmake/llvm/ -DLLVM_LIBRARY_DIR=/usr/lib64/llvm13/lib/ -DWasmEdge_LIB=/usr/local/lib64/libwasmedge.a &&
make -j8 VERBOSE=1 &&
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

echo "END INSIDE CONTAINER"
