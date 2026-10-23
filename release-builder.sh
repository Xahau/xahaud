#!/bin/bash
# We use set -e and bash with -u to bail on first non zero exit code of any
# processes launched or upon any unbound variable.
# We use set -x to print commands before running them to help with
# debugging.

set -ex

echo "START BUILDING (HOST)"

echo "Cleaning previously built binary"
rm -f release-build/xahaud

BUILD_CORES=$(echo "scale=0 ; $(nproc) / 1.337" | bc)

if [[ "$GITHUB_REPOSITORY" == "" ]]; then
  #Default
  BUILD_CORES=${BUILD_CORES:-8}
fi

# Ensure still works outside of GH Actions by setting these to /dev/null
# GA will run this script and then delete it at the end of the job
JOB_CLEANUP_SCRIPT=${JOB_CLEANUP_SCRIPT:-/dev/null}
NORMALIZED_WORKFLOW=$(echo "$GITHUB_WORKFLOW" | tr -c 'a-zA-Z0-9' '-')
NORMALIZED_REF=$(echo "$GITHUB_REF" | tr -c 'a-zA-Z0-9' '-')
CONTAINER_NAME="xahaud_cached_builder_${NORMALIZED_WORKFLOW}-${NORMALIZED_REF}"

echo "-- BUILD CORES:       $BUILD_CORES"
echo "-- GITHUB_REPOSITORY: $GITHUB_REPOSITORY"
echo "-- GITHUB_SHA:        $GITHUB_SHA"
echo "-- GITHUB_RUN_NUMBER: $GITHUB_RUN_NUMBER"
echo "-- CONTAINER_NAME:    $CONTAINER_NAME"

which docker 2>/dev/null 2>/dev/null
if [ "$?" -eq "1" ]; then
  echo 'Docker not found. Install it first.'
  exit 1
fi

stat .git 2>/dev/null 2>/dev/null
if [ "$?" -eq "1" ]; then
  echo 'Run this inside the source directory. (.git dir not found).'
  exit 1
fi

STATIC_CONTAINER=$(docker ps -a | grep $CONTAINER_NAME | wc -l)

CACHE_VOLUME_NAME="xahau-release-builder-cache"

# if [[ "$STATIC_CONTAINER" -gt "0" && "$GITHUB_REPOSITORY" != "" ]]; then
if false; then
  echo "Static container, execute in static container to have max. cache"
  docker start $CONTAINER_NAME
  docker exec -i $CONTAINER_NAME /hbb_exe/activate-exec bash -c "source /opt/rh/gcc-toolset-11/enable && bash -x /io/build-core.sh '$GITHUB_REPOSITORY' '$GITHUB_SHA' '$BUILD_CORES' '$GITHUB_RUN_NUMBER'"
  docker stop $CONTAINER_NAME
else
  echo "No static container, build on temp container"
  rm -rf release-build
  mkdir -p release-build

  docker volume create $CACHE_VOLUME_NAME

  # Create inline Dockerfile with environment setup for build-full.sh
  DOCKERFILE_CONTENT=$(
    cat <<'DOCKERFILE_EOF'
FROM ghcr.io/phusion/holy-build-box:4.0.1-amd64

ARG BUILD_CORES=8

# Enable repositories and install dependencies
RUN /hbb_exe/activate-exec bash -c "dnf install -y epel-release && \
    dnf config-manager --set-enabled powertools || dnf config-manager --set-enabled crb && \
    dnf install -y --enablerepo=devel \
        wget git \
        gcc-toolset-11-gcc-c++ gcc-toolset-11-binutils gcc-toolset-11-libatomic-devel \
        lz4 lz4-devel \
        ncurses-static ncurses-devel \
        snappy snappy-devel \
        zlib zlib-devel zlib-static \
        libasan \
        python3.11 python3.11-pip \
        ccache \
        ninja-build \
        mold \
        patch \
        glibc-devel glibc-static \
        libxml2-devel \
        autoconf \
        automake \
        texinfo \
        libtool && \
    dnf clean all"

# Install Conan 2 and CMake
RUN /hbb_exe/activate-exec pip3.11 install "conan>=2.0,<3.0" && \
    /hbb_exe/activate-exec wget -q https://github.com/Kitware/CMake/releases/download/v3.25.3/cmake-3.25.3-linux-x86_64.tar.gz -O cmake.tar.gz && \
    mkdir cmake && \
    tar -xzf cmake.tar.gz --strip-components=1 -C cmake && \
    rm cmake.tar.gz

# Boost headers for WasmEdge only (its CMake does find_package(Boost); on Linux
# the library is header-only INTERFACE usage, nothing is linked). Without a
# system Boost it would FetchContent a 1.76 tarball from a dead jfrog URL.
# The application itself uses Conan-provided Boost via the toolchain.
RUN /hbb_exe/activate-exec bash -c "cd /tmp && \
    wget -q https://archives.boost.io/release/1.86.0/source/boost_1_86_0.tar.gz -O boost.tar.gz && \
    tar -xzf boost.tar.gz --strip-components=1 -C /usr/local/include boost_1_86_0/boost && \
    rm -f boost.tar.gz"

ENV CMAKE_EXE_LINKER_FLAGS="-static-libstdc++"

ENV WasmEdge_LIB=/usr/local/lib64/libwasmedge.a

ENV CC='ccache gcc'
ENV CXX='ccache g++'

# Build and install WasmEdge (static version)
# Note: Conan only provides WasmEdge with shared library linking.
# xahaud only uses the interpreter (VM/ModuleInstance/Statistics C API), never
# the AOT compiler, so the LLVM-based runtime is disabled. This removes the
# LLVM 14 static libs, the LLD source build and the Boost b2 build.
RUN cd /tmp && \
    ( wget -nc -q https://github.com/WasmEdge/WasmEdge/archive/refs/tags/0.11.2.zip; unzip -o 0.11.2.zip; ) && \
    cd WasmEdge-0.11.2 && \
    ( mkdir -p build; echo "" ) && \
    cd build && \
    /hbb_exe/activate-exec bash -c "source /opt/rh/gcc-toolset-11/enable && \
    ln -sf /opt/rh/gcc-toolset-11/root/usr/bin/ar /usr/bin/ar && \
    ln -sf /opt/rh/gcc-toolset-11/root/usr/bin/ranlib /usr/bin/ranlib && \
    echo '=== Binutils version check ===' && \
    ar --version | head -1 && \
    ranlib --version | head -1 && \
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DWASMEDGE_BUILD_SHARED_LIB=OFF \
        -DWASMEDGE_BUILD_STATIC_LIB=ON \
        -DWASMEDGE_BUILD_AOT_RUNTIME=OFF \
        -DWASMEDGE_BUILD_TOOLS=OFF \
        -DWASMEDGE_FORCE_DISABLE_LTO=ON \
        -DWASMEDGE_BUILD_PLUGINS=OFF \
        -DBoost_NO_BOOST_CMAKE=ON \
        -DCMAKE_EXE_LINKER_FLAGS=\"\$CMAKE_EXE_LINKER_FLAGS\" \
        && \
    make -j${BUILD_CORES} install" && \
    cp -r include/api/wasmedge /usr/include/ && \
    cd /tmp && rm -rf WasmEdge* 0.11.2.zip

# Set environment variables
ENV PATH=/usr/local/bin:$PATH

# Configure ccache and Conan 2
# NOTE: Using echo commands instead of heredocs because heredocs in Docker RUN commands are finnicky
RUN /hbb_exe/activate-exec bash -c "ccache -M 100G && \
    ccache -o cache_dir=/cache/ccache && \
    ccache -o compiler_check=content && \
    ccache -o direct_mode=true && \
    mkdir -p ~/.conan2 /cache/conan2 /cache/conan2_download /cache/conan2_sources && \
    echo 'core.cache:storage_path=/cache/conan2' > ~/.conan2/global.conf && \
    echo 'core.download:download_cache=/cache/conan2_download' >> ~/.conan2/global.conf && \
    echo 'core.sources:download_cache=/cache/conan2_sources' >> ~/.conan2/global.conf && \
    conan profile detect --force && \
    echo '[settings]' > ~/.conan2/profiles/default && \
    echo 'arch=x86_64' >> ~/.conan2/profiles/default && \
    echo 'build_type=Release' >> ~/.conan2/profiles/default && \
    echo 'compiler=gcc' >> ~/.conan2/profiles/default && \
    echo 'compiler.cppstd=20' >> ~/.conan2/profiles/default && \
    echo 'compiler.libcxx=libstdc++11' >> ~/.conan2/profiles/default && \
    echo 'compiler.version=11' >> ~/.conan2/profiles/default && \
    echo 'os=Linux' >> ~/.conan2/profiles/default && \
    echo '' >> ~/.conan2/profiles/default && \
    echo '[conf]' >> ~/.conan2/profiles/default && \
    echo '# Force building from source for packages with binary compatibility issues' >> ~/.conan2/profiles/default && \
    echo '*:tools.system.package_manager:mode=build' >> ~/.conan2/profiles/default && \
    ln -s ../../bin/ccache /usr/lib64/ccache/g++ && \
    ln -s ../../bin/ccache /usr/lib64/ccache/c++"

DOCKERFILE_EOF
  )

  # Build custom Docker image
  IMAGE_NAME="xahaud-builder:latest"
  echo "Building custom Docker image with dependencies..."
  echo "$DOCKERFILE_CONTENT" | docker build --build-arg BUILD_CORES="$BUILD_CORES" -t "$IMAGE_NAME" - || exit 1

  if [[ "$GITHUB_REPOSITORY" == "" ]]; then
    # Non GH, local building
    echo "Non-GH runner, local building, temp container"
    docker run -i --user 0:$(id -g) --rm -v /data/builds:/data/builds -v $(pwd):/io -v "$CACHE_VOLUME_NAME":/cache --network host "$IMAGE_NAME" /hbb_exe/activate-exec bash -c "source /opt/rh/gcc-toolset-11/enable && bash -x /io/build-full.sh '$GITHUB_REPOSITORY' '$GITHUB_SHA' '$BUILD_CORES' '$GITHUB_RUN_NUMBER'"
  else
    # GH Action, runner
    echo "GH Action, runner, clean & re-create create persistent container"
    docker rm -f $CONTAINER_NAME
    echo "echo 'Stopping container: $CONTAINER_NAME'" >>"$JOB_CLEANUP_SCRIPT"
    echo "docker stop --time=15 \"$CONTAINER_NAME\" || echo 'Failed to stop container or container not running'" >>"$JOB_CLEANUP_SCRIPT"
    docker run -di --user 0:$(id -g) --name $CONTAINER_NAME -v /data/builds:/data/builds -v $(pwd):/io -v "$CACHE_VOLUME_NAME":/cache --network host "$IMAGE_NAME" /hbb_exe/activate-exec bash
    docker exec -i $CONTAINER_NAME /hbb_exe/activate-exec bash -c "source /opt/rh/gcc-toolset-11/enable && bash -x /io/build-full.sh '$GITHUB_REPOSITORY' '$GITHUB_SHA' '$BUILD_CORES' '$GITHUB_RUN_NUMBER'"
    docker stop $CONTAINER_NAME
  fi
fi

echo "DONE BUILDING (HOST)"
