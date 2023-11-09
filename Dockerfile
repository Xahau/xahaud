# docker build -t transia/xahaud-hbb-deps .
# Use the Holy Build Box image as the base image
FROM ghcr.io/foobarwidget/holy-build-box-x64

# Set the user to root and the group to the current user's group ID
# Note: You will need to replace `$(id -g)` with the actual group ID or remove it if not needed
# USER root:$(id -g)

# Install wget and other necessary tools
RUN /hbb_exe/activate-exec bash -c "yum update -y && \
    yum install -y wget lz4 lz4-devel git llvm13-static.x86_64 llvm13-devel.x86_64 devtoolset-10-binutils zlib-static ncurses-static \
    devtoolset-7-gcc-c++ \
    devtoolset-9-gcc-c++ \
    devtoolset-10-gcc-c++ \
    snappy snappy-devel \
    zlib zlib-devel \
    lz4-devel \
    libasan && \
    yum clean all && \
    rm -rf /var/cache/yum"

COPY . /io/

# Create and set the working directory to /io
WORKDIR /io

# Set file permissions mask
RUN /hbb_exe/activate-exec bash -c "umask 0000"

ENV ZSTD_VERSION="1.1.3" \
    BOOST_ROOT="/usr/local/src/boost_1_75_0" \
    Boost_LIBRARY_DIRS="/usr/local/lib" \
    BOOST_INCLUDEDIR="/usr/local/src/boost_1_75_0" \
    LLVM_DIR="/usr/lib64/llvm13/lib/cmake/llvm/" \
    LLVM_LIBRARY_DIR="/usr/lib64/llvm13/lib/"

# Install ZStd 1.1.3
RUN /hbb_exe/activate-exec bash -c "source /opt/rh/devtoolset-7/enable && echo '-- Install ZStd 1.1.3 --' && \
    yum install epel-release -y && \
    wget -nc -q -O zstd-${ZSTD_VERSION}.tar.gz https://github.com/facebook/zstd/archive/v${ZSTD_VERSION}.tar.gz && \
    tar xzvf zstd-${ZSTD_VERSION}.tar.gz && \
    cd zstd-${ZSTD_VERSION} && \
    make -j$(nproc) install && \
    cd .. && \
    rm -rf zstd-${ZSTD_VERSION} zstd-${ZSTD_VERSION}.tar.gz"

# Install Cmake 3.23.1
RUN /hbb_exe/activate-exec bash -c "source /opt/rh/devtoolset-7/enable && echo '-- Install Cmake 3.23.1 --' && \
    wget -nc -q https://github.com/Kitware/CMake/releases/download/v3.23.1/cmake-3.23.1-linux-x86_64.tar.gz && \
    tar -xzf cmake-3.23.1-linux-x86_64.tar.gz -C /hbb/ && \
    rm cmake-3.23.1-linux-x86_64.tar.gz"

# Install Boost 1.75.0
RUN /hbb_exe/activate-exec bash -c "source /opt/rh/devtoolset-7/enable && echo '-- Install Boost 1.75.0 --' && \
    wget -nc -q https://boostorg.jfrog.io/artifactory/main/release/1.75.0/source/boost_1_75_0.tar.gz && \
    tar -xzf boost_1_75_0.tar.gz && \
    cd boost_1_75_0 && \
    ./bootstrap.sh && \
    ./b2 link=static -j$(nproc) && \
    ./b2 install && \
    cd ../ && \
    rm -rf boost_1_75_0 boost_1_75_0.tar.gz"

# Install Protobuf 3.20.0
RUN /hbb_exe/activate-exec bash -c "source /opt/rh/devtoolset-7/enable && echo '-- Install Protobuf 3.20.0 --' && \
    wget -nc -q https://github.com/protocolbuffers/protobuf/releases/download/v3.20.0/protobuf-all-3.20.0.tar.gz && \
    tar -xzf protobuf-all-3.20.0.tar.gz && \
    cd protobuf-3.20.0/ && \
    ./autogen.sh && \
    ./configure --prefix=/usr --disable-shared link=static && \
    make -j$(nproc) && \
    make install && \
    cd .. && \
    rm -rf protobuf-3.20.0 protobuf-all-3.20.0.tar.gz"

# Build LLD
RUN /hbb_exe/activate-exec bash -c "source /opt/rh/devtoolset-7/enable && echo '-- Build LLD --' && \
    ln /usr/bin/llvm-config-13 /usr/bin/llvm-config && \
    mv /opt/rh/devtoolset-9/root/usr/bin/ar /opt/rh/devtoolset-9/root/usr/bin/ar-9 && \
    ln /opt/rh/devtoolset-10/root/usr/bin/ar /opt/rh/devtoolset-9/root/usr/bin/ar && \
    wget -nc -q https://github.com/llvm/llvm-project/releases/download/llvmorg-13.0.1/lld-13.0.1.src.tar.xz && \
    wget -nc -q https://github.com/llvm/llvm-project/releases/download/llvmorg-13.0.1/libunwind-13.0.1.src.tar.xz && \
    tar -xf lld-13.0.1.src.tar.xz && \
    tar -xf libunwind-13.0.1.src.tar.xz && \
    cp -r libunwind-13.0.1.src/include libunwind-13.0.1.src/src lld-13.0.1.src/ && \
    cd lld-13.0.1.src && \
    rm -rf build CMakeCache.txt && \
    mkdir build && \
    cd build && \
    cmake .. -DLLVM_LIBRARY_DIR=/usr/lib64/llvm13/lib/ -DCMAKE_INSTALL_PREFIX=/usr/lib64/llvm13/ -DCMAKE_BUILD_TYPE=Release && \
    make -j$(nproc) install && \
    ln -s /usr/lib64/llvm13/lib/include/lld /usr/include/lld && \
    cp /usr/lib64/llvm13/lib/liblld*.a /usr/local/lib/ && \
    cd ../../ && \
    rm -rf lld-13.0.1.src libunwind-13.0.1.src lld-13.0.1.src.tar.xz libunwind-13.0.1.src.tar.xz"

# Build WasmEdge
RUN /hbb_exe/activate-exec bash -c "source /opt/rh/devtoolset-9/enable && echo '-- Build WasmEdge --' && \
    wget -nc -q https://github.com/WasmEdge/WasmEdge/archive/refs/tags/0.11.2.zip && \
    unzip -o 0.11.2.zip && \
    cd WasmEdge-0.11.2 && \
    mkdir build && \
    cd build && \
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DWASMEDGE_BUILD_SHARED_LIB=OFF \
        -DWASMEDGE_BUILD_STATIC_LIB=ON \
        -DWASMEDGE_BUILD_AOT_RUNTIME=ON \
        -DWASMEDGE_FORCE_DISABLE_LTO=ON \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DWASMEDGE_LINK_LLVM_STATIC=ON \
        -DWASMEDGE_BUILD_PLUGINS=OFF \
        -DWASMEDGE_LINK_TOOLS_STATIC=ON \
        -DBoost_NO_BOOST_CMAKE=ON -DLLVM_DIR=/usr/lib64/llvm13/lib/cmake/llvm/ -DLLVM_LIBRARY_DIR=/usr/lib64/llvm13/lib/ && \
    make -j$(nproc) install && \
    cp -r include/api/wasmedge /usr/include/ && \
    cd ../../ && \
    rm -rf WasmEdge-0.11.2 0.11.2.zip"

# Set the entrypoint to the activate-exec script with bash
# This will be the default command that runs when the container starts
ENTRYPOINT ["/hbb_exe/activate-exec"]
CMD ["bash"]