#[===================================================================[
   NIH dep: openssl
#]===================================================================]

# Include modules
include(ExternalProject)
include(CheckCCompilerFlag)

check_c_compiler_flag("-mavx512f" COMPILER_SUPPORTS_AVX512)
option(ENABLE_AVX512 "Enable AVX512 instructions" ${COMPILER_SUPPORTS_AVX512})

set(OPENSSL_NISTP "")
set(OPENSSL_AVX512 "")

if(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
        set(OPENSSL_PLATFORM "darwin64-arm64-cc")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
        set(OPENSSL_PLATFORM "darwin64-x86_64-cc")
    else()
        message(FATAL_ERROR "Unsupported macOS architecture: ${CMAKE_SYSTEM_PROCESSOR}")
    endif()
elseif(UNIX AND NOT APPLE)
    message(STATUS "ARCH: ${CMAKE_SYSTEM_PROCESSOR}")
    set(OPENSSL_PLATFORM "linux-x86_64")
    set(OPENSSL_NISTP "enable-ec_nistp_64_gcc_128")
    set(OPENSSL_AVX512 "-march=skylake-avx512")
else()
    message(FATAL_ERROR "Unsupported platform")
endif()

ExternalProject_Add(openssl_src
    PREFIX ${nih_cache_path}
    GIT_REPOSITORY https://github.com/openssl/openssl.git
    GIT_TAG OpenSSL_1_1_1u
    CONFIGURE_COMMAND
      ${nih_cache_path}/src/openssl_src/Configure
      -DBUILD_SHARED_LIBS=OFF
      -DBUILD_STATIC_LIBS=ON
      ${OPENSSL_PLATFORM}
      ${OPENSSL_NISTP}
      ${OPENSSL_AVX512}
      enable-rmd160
      no-ssl2
    LOG_BUILD ON
    LOG_CONFIGURE ON
    LOG_DOWNLOAD ON
    BUILD_IN_SOURCE 1
    BUILD_COMMAND $(MAKE) CFLAGS=${OPENSSL_CFLAGS}
    TEST_COMMAND ""
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS
      <BINARY_DIR>/libssl.a
      <BINARY_DIR>/libcrypto.a
)

ExternalProject_Get_Property (openssl_src BINARY_DIR)
set (openssl_src_BINARY_DIR "${BINARY_DIR}")

add_library (OpenSSL::SSL STATIC IMPORTED GLOBAL)
add_library (OpenSSL::Crypto STATIC IMPORTED GLOBAL)

add_dependencies(OpenSSL::SSL openssl_src)
add_dependencies(OpenSSL::Crypto openssl_src)
execute_process(
    COMMAND
        mkdir -p "${openssl_src_BINARY_DIR}/include"
)

set_target_properties(OpenSSL::SSL PROPERTIES
    IMPORTED_LOCATION_DEBUG
      "${openssl_src_BINARY_DIR}/libssl.a"
    IMPORTED_LOCATION_RELEASE
      "${openssl_src_BINARY_DIR}/libssl.a"
    INTERFACE_INCLUDE_DIRECTORIES
      "${openssl_src_BINARY_DIR}/include"
)

set_target_properties(OpenSSL::Crypto PROPERTIES
    IMPORTED_LOCATION_DEBUG
      "${openssl_src_BINARY_DIR}/libcrypto.a"
    IMPORTED_LOCATION_RELEASE
      "${openssl_src_BINARY_DIR}/libcrypto.a"
    INTERFACE_INCLUDE_DIRECTORIES
      "${openssl_src_BINARY_DIR}/include"
)

target_link_libraries (ripple_libs
  INTERFACE
    OpenSSL::SSL
    OpenSSL::Crypto)
#[=========================================================[
   https://gitlab.kitware.com/cmake/cmake/issues/16885
   depending on how openssl is built, it might depend
   on zlib. In fact, the openssl find package should
   figure this out for us, but it does not currently...
   so let's add zlib ourselves to the lib list
   TODO: investigate linking to static zlib for static
   build option
#]=========================================================]
find_package (ZLIB)
set (has_zlib FALSE)
if (TARGET ZLIB::ZLIB)
  set_target_properties(OpenSSL::Crypto PROPERTIES
    INTERFACE_LINK_LIBRARIES ZLIB::ZLIB)
  set (has_zlib TRUE)
endif ()

# Compiler flags for your application
if(ENABLE_AVX512 AND COMPILER_SUPPORTS_AVX512)
  # Allow the compiler to use AVX512 instructions where appropriate
  add_compile_options(-mavx512f)
  add_compile_options(-march=skylake-avx512)
endif()
add_link_options(-lssl -lcrypto)