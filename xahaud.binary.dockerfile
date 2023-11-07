# docker build -f xahaud.binary.dockerfile -t transia/xahaud-binary .
# Use a base image that includes the necessary build tools and libraries
FROM transia/xahaud-hbb-deps

ENV GITHUB_RUN_NUMBER=${GITHUB_RUN_NUMBER}

# Copy the project source code into the container
COPY . /io

# Set the working directory
WORKDIR /io

# Create directory for certificates
RUN mkdir -p src/certs

# Download the certificate bundle
RUN curl --silent -k https://raw.githubusercontent.com/RichardAH/rippled-release-builder/main/ca-bundle/certbundle.h -o src/certs/certbundle.h

# Check if certbundle.h needs to be included in RegisterSSLCerts.cpp
RUN ./ssl_script.sh

# Set file permissions
RUN /hbb_exe/activate-exec bash -c "umask 0000"

# Modify Rocksdb.cmake
# RUN cp Builds/CMake/deps/Rocksdb.cmake Builds/CMake/deps/Rocksdb.cmake.old
RUN perl -i -pe "s/^(\\s*)-DBUILD_SHARED_LIBS=OFF/\\1-DBUILD_SHARED_LIBS=OFF\\n\\1-DROCKSDB_BUILD_SHARED=OFF/g" Builds/CMake/deps/Rocksdb.cmake

# Replace WasmEdge.cmake with a new configuration
RUN /hbb_exe/activate-exec bash -c "echo -e 'find_package(LLVM REQUIRED CONFIG)\nmessage(STATUS \"Found LLVM ${LLVM_PACKAGE_VERSION}\")\nmessage(STATUS \"Using LLVMConfig.cmake in: \${LLVM_DIR}\")\nadd_library (wasmedge STATIC IMPORTED GLOBAL)\nset_target_properties(wasmedge PROPERTIES IMPORTED_LOCATION \${WasmEdge_LIB})\ntarget_link_libraries (ripple_libs INTERFACE wasmedge)\nadd_library (NIH::WasmEdge ALIAS wasmedge)\nmessage(\"WasmEdge DONE\")' > Builds/CMake/deps/WasmEdge.cmake"

# Update BuildInfo.cpp with the current date and Git information
RUN sed -i s/\"0.0.0\"/\"$(date +%Y).$(date +%-m).$(date +%-d)-$(git rev-parse --abbrev-ref HEAD)+${GITHUB_RUN_NUMBER}\"/g src/ripple/protocol/impl/BuildInfo.cpp

# Create build directory
RUN /hbb_exe/activate-exec bash -c "mkdir -p release-build"

# Configure the build with CMake
RUN /hbb_exe/activate-exec bash -c "cd release-build && source /opt/rh/devtoolset-10/enable && cmake .. -DCMAKE_BUILD_TYPE=Release -DBoost_NO_BOOST_CMAKE=ON -DLLVM_DIR=/usr/lib64/llvm13/lib/cmake/llvm/ -DLLVM_LIBRARY_DIR=/usr/lib64/llvm13/lib/ -DWasmEdge_LIB=/usr/local/lib64/libwasmedge.a && make -j$(nproc) VERBOSE=1"

# Strip the binary and rename it
RUN /hbb_exe/activate-exec bash -c "cd release-build && strip -s rippled && mv rippled xahaud"

# Create release information
RUN cd release-build && \
    echo "Build host: $(hostname)" > release.info && \
    echo "Build date: $(date)" >> release.info && \
    echo "Build md5: $(md5sum xahaud)" >> release.info && \
    echo "Git remotes:" >> release.info && \
    git remote -v >> release.info && \
    echo "Git status:" >> release.info && \
    git status -v >> release.info && \
    echo "Git log [last 20]:" >> release.info && \
    git log -n 20 >> release.info

ENTRYPOINT ["/hbb_exe/activate-exec"]
CMD ["bash"]