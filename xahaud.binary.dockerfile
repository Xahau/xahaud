FROM transia/xahaud-hbb-deps

ARG GITHUB_BRANCH
ARG GITHUB_RUN_NUMBER

# Copy the project source code into the container
COPY . /io

# Set the working directory
WORKDIR /io

# Set file permissions
RUN umask 0000

# Create directory for certificates, download the certificate bundle, and check if certbundle.h needs to be included in RegisterSSLCerts.cpp
RUN mkdir -p src/certs && \
    curl --silent -k https://raw.githubusercontent.com/RichardAH/rippled-release-builder/main/ca-bundle/certbundle.h -o src/certs/certbundle.h && \
    ./ssl_script.sh

# Modify Rocksdb.cmake, replace WasmEdge.cmake with a new configuration, and update BuildInfo.cpp with the current date and Git information
RUN perl -i -pe "s/^(\\s*)-DBUILD_SHARED_LIBS=OFF/\\1-DBUILD_SHARED_LIBS=OFF\\n\\1-DROCKSDB_BUILD_SHARED=OFF/g" Builds/CMake/deps/Rocksdb.cmake && \
    echo -e 'find_package(LLVM REQUIRED CONFIG)\nmessage(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")\nmessage(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")\nadd_library (wasmedge STATIC IMPORTED GLOBAL)\nset_target_properties(wasmedge PROPERTIES IMPORTED_LOCATION ${WasmEdge_LIB})\ntarget_link_libraries (ripple_libs INTERFACE wasmedge)\nadd_library (NIH::WasmEdge ALIAS wasmedge)\nmessage("WasmEdge DONE")' > Builds/CMake/deps/WasmEdge.cmake && \
    sed -i s/"0.0.0"/"$(date +%Y).$(date +%-m).$(date +%-d)-${GITHUB_BRANCH}+${GITHUB_RUN_NUMBER}"/g src/ripple/protocol/impl/BuildInfo.cpp

# Create build directory, configure the build with CMake, and build the project
RUN mkdir -p release-build && \
    cd release-build && source /opt/rh/devtoolset-10/enable && cmake .. -DCMAKE_BUILD_TYPE=Release -DBoost_NO_BOOST_CMAKE=ON -DLLVM_DIR=/usr/lib64/llvm13/lib/cmake/llvm/ -DLLVM_LIBRARY_DIR=/usr/lib64/llvm13/lib/ -DWasmEdge_LIB=/usr/local/lib64/libwasmedge.a && make -j$(nproc) VERBOSE=1

# Strip the binary, rename it, and create release information
RUN cd release-build && \
    strip -s rippled && \
    mv rippled xahaud && \
    echo "Build host: $(hostname)" > release.info && \
    echo "Build date: $(date)" >> release.info && \
    echo "Build md5: $(md5sum xahaud)" >> release.info && \
    echo "Git remotes:" >> release.info && \
    git remote -v >> release.info && \
    echo "Git status:" >> release.info && \
    git status -v >> release.info && \
    echo "Git log [last 20]:" >> release.info && \
    git log -n 20 >> release.info

ENTRYPOINT /bin/bash