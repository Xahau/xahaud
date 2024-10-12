#[===================================================================[
   NIH dep: openssl
#]===================================================================]

# Include modules
include(ExternalProject)
include(CheckCCompilerFlag)

check_c_compiler_flag("-mavx512f" COMPILER_SUPPORTS_AVX512)
option(ENABLE_AVX512 "Enable AVX512 instructions" ${COMPILER_SUPPORTS_AVX512})

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(OPENSSL_PLATFORM "linux-x86_64")
    if(ENABLE_AVX512 AND COMPILER_SUPPORTS_AVX512)
        set(OPENSSL_CFLAGS "-mavx512f")
    else()
        set(OPENSSL_CFLAGS "")
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
    set(OPENSSL_PLATFORM "darwin64-arm64-cc")
  else()
    set(OPENSSL_PLATFORM "darwin64-x86_64-cc")
    if(ENABLE_AVX512 AND COMPILER_SUPPORTS_AVX512)
        set(OPENSSL_CFLAGS "-mavx512f")
    else()
        set(OPENSSL_CFLAGS "")
    endif()
  endif()
elseif(WIN32)
    # For Windows, assuming MSVC
    # OpenSSL requires Perl and NASM for building
    find_program(PERL perl)
    if(NOT PERL)
        message(FATAL_ERROR "Perl not found. Perl is required to build OpenSSL on Windows.")
    endif()
    find_program(NASM nasm)
    if(NOT NASM)
        message(FATAL_ERROR "NASM not found. NASM is required to build OpenSSL on Windows.")
    endif()

    if(ENABLE_AVX512 AND COMPILER_SUPPORTS_AVX512)
        set(OPENSSL_CL "/arch:AVX512")
    else()
        set(OPENSSL_CL "")
    endif()

    set(OPENSSL_PLATFORM "VC-WIN64A")
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

ExternalProject_Add(openssl_src
    PREFIX ${nih_cache_path}
    GIT_REPOSITORY https://github.com/openssl/openssl.git
    GIT_TAG openssl-3.3.2
    CONFIGURE_COMMAND
      ${nih_cache_path}/src/openssl_src/Configure
      -DBUILD_SHARED_LIBS=OFF
      -DBUILD_STATIC_LIBS=ON
      enable-rmd160
      no-ssl2
      ${OPENSSL_PLATFORM}
    LOG_BUILD ON
    LOG_CONFIGURE ON
    LOG_DOWNLOAD ON
    BUILD_IN_SOURCE 1
    BUILD_COMMAND $(MAKE) CFLAGS=${OPENSSL_CFLAGS}
    TEST_COMMAND ""
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS
      <BINARY_DIR>/libcrypto.a
      <BINARY_DIR>/libssl.a
)

ExternalProject_Get_Property (openssl_src BINARY_DIR)
set (openssl_src_BINARY_DIR "${BINARY_DIR}")

add_library (OpenSSL::Crypto STATIC IMPORTED GLOBAL)
add_library (OpenSSL::SSL STATIC IMPORTED GLOBAL)

add_dependencies(OpenSSL::Crypto openssl_src)
add_dependencies(OpenSSL::SSL openssl_src)

if(MSVC)
    # On Linux/macOS, .a or .so file extensions
    set_target_properties(OpenSSL::Crypto PROPERTIES
        IMPORTED_LOCATION_DEBUG
          "${openssl_src_BINARY_DIR}/libcrypto.lib"
        IMPORTED_LOCATION_RELEASE
          "${openssl_src_BINARY_DIR}/libcrypto.lib"
        INTERFACE_INCLUDE_DIRECTORIES
          "${openssl_src_BINARY_DIR}/include"
    )
    set_target_properties(OpenSSL::SSL PROPERTIES
        IMPORTED_LOCATION_DEBUG
          "${openssl_src_BINARY_DIR}/libssl.lib"
        IMPORTED_LOCATION_RELEASE
          "${openssl_src_BINARY_DIR}/libssl.lib"
        INTERFACE_INCLUDE_DIRECTORIES
          "${openssl_src_BINARY_DIR}/include"
    )
else()
    # On Linux/macOS, .a or .so file extensions
    set_target_properties(OpenSSL::Crypto PROPERTIES
        IMPORTED_LOCATION_DEBUG
          "${openssl_src_BINARY_DIR}/libcrypto.a"
        IMPORTED_LOCATION_RELEASE
          "${openssl_src_BINARY_DIR}/libcrypto.a"
        INTERFACE_INCLUDE_DIRECTORIES
          "${openssl_src_BINARY_DIR}/include"
    )
    set_target_properties(OpenSSL::SSL PROPERTIES
        IMPORTED_LOCATION_DEBUG
          "${openssl_src_BINARY_DIR}/libssl.a"
        IMPORTED_LOCATION_RELEASE
          "${openssl_src_BINARY_DIR}/libssl.a"
        INTERFACE_INCLUDE_DIRECTORIES
          "${openssl_src_BINARY_DIR}/include"
    )
endif()

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
