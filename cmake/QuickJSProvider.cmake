set(XAHAU_QUICKJS_PROVIDER_BUNDLE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/external/quickjs-provider"
  CACHE PATH
  "Directory containing the sealed QuickJS provider WASM and generated manifests")
get_filename_component(XAHAU_QUICKJS_PROVIDER_BUNDLE_DIR
  "${XAHAU_QUICKJS_PROVIDER_BUNDLE_DIR}"
  ABSOLUTE
  BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(XAHAU_QUICKJS_PROVIDER_MANIFEST
  "${XAHAU_QUICKJS_PROVIDER_BUNDLE_DIR}/quickjs_contract.manifest.cmake")
set(XAHAU_QUICKJS_PROVIDER_PROFILE_LOCK
  "${XAHAU_QUICKJS_PROVIDER_BUNDLE_DIR}/quickjs_contract.manifest.json")

if(NOT EXISTS "${XAHAU_QUICKJS_PROVIDER_MANIFEST}")
  message(FATAL_ERROR
    "Missing generated QuickJS provider manifest: "
    "${XAHAU_QUICKJS_PROVIDER_MANIFEST}")
endif()
include("${XAHAU_QUICKJS_PROVIDER_MANIFEST}")

if(NOT XAHAU_QUICKJS_MANIFEST_SCHEMA STREQUAL
    "xahau.quickjs.runtime-profile-lock.v1")
  message(FATAL_ERROR
    "Unsupported QuickJS provider manifest schema: "
    "${XAHAU_QUICKJS_MANIFEST_SCHEMA}")
endif()

file(SHA256
  "${XAHAU_QUICKJS_PROVIDER_PROFILE_LOCK}"
  XAHAU_QUICKJS_ACTUAL_MANIFEST_SHA256)
if(NOT XAHAU_QUICKJS_ACTUAL_MANIFEST_SHA256 STREQUAL
    XAHAU_QUICKJS_MANIFEST_SHA256)
  message(FATAL_ERROR
    "QuickJS provider JSON manifest does not match its CMake projection")
endif()

if(DEFINED wasmtime_VERSION_STRING AND
   NOT wasmtime_VERSION_STRING VERSION_EQUAL XAHAU_QUICKJS_WASMTIME_VERSION)
  message(FATAL_ERROR
    "QuickJS provider requires Wasmtime ${XAHAU_QUICKJS_WASMTIME_VERSION}, "
    "but CMake resolved ${wasmtime_VERSION_STRING}")
endif()

function(xahau_quickjs_hex_to_cpp_bytes input output)
  string(LENGTH "${input}" input_length)
  if(NOT input_length EQUAL 64 OR NOT input MATCHES "^[0-9a-fA-F]+$")
    message(FATAL_ERROR "QuickJS identity must be exactly 32 hexadecimal bytes")
  endif()
  set(bytes "")
  foreach(index RANGE 0 62 2)
    string(SUBSTRING "${input}" ${index} 2 byte)
    list(APPEND bytes "0x${byte}")
  endforeach()
  string(JOIN ", " bytes ${bytes})
  set(${output} "${bytes}" PARENT_SCOPE)
endfunction()

xahau_quickjs_hex_to_cpp_bytes(
  "${XAHAU_QUICKJS_PROVIDER_SHA256}"
  XAHAU_QUICKJS_PROVIDER_SHA256_CPP)
xahau_quickjs_hex_to_cpp_bytes(
  "${XAHAU_QUICKJS_BYTECODE_ABI_ID}"
  XAHAU_QUICKJS_BYTECODE_ABI_ID_CPP)
xahau_quickjs_hex_to_cpp_bytes(
  "${XAHAU_QUICKJS_RUNTIME_PROFILE_ID}"
  XAHAU_QUICKJS_RUNTIME_PROFILE_ID_CPP)

set(XAHAU_QUICKJS_GENERATED_INCLUDE_DIR
  "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY
  "${XAHAU_QUICKJS_GENERATED_INCLUDE_DIR}/xrpld/app/hook/detail")
configure_file(
  "${CMAKE_CURRENT_LIST_DIR}/QuickJSProviderProfile.h.in"
  "${XAHAU_QUICKJS_GENERATED_INCLUDE_DIR}/xrpld/app/hook/detail/QuickJSProviderProfile.h"
  @ONLY)

set(XAHAU_QUICKJS_PROVIDER_WASM
  "${XAHAU_QUICKJS_PROVIDER_BUNDLE_DIR}/${XAHAU_QUICKJS_PROVIDER_FILE}")
if(EXISTS "${XAHAU_QUICKJS_PROVIDER_WASM}")
  file(SHA256
    "${XAHAU_QUICKJS_PROVIDER_WASM}"
    XAHAU_QUICKJS_ACTUAL_PROVIDER_SHA256)
  file(SIZE
    "${XAHAU_QUICKJS_PROVIDER_WASM}"
    XAHAU_QUICKJS_ACTUAL_PROVIDER_SIZE)
  if(NOT XAHAU_QUICKJS_ACTUAL_PROVIDER_SHA256 STREQUAL
      XAHAU_QUICKJS_PROVIDER_SHA256 OR
     NOT XAHAU_QUICKJS_ACTUAL_PROVIDER_SIZE EQUAL
      XAHAU_QUICKJS_PROVIDER_SIZE)
    message(FATAL_ERROR
      "QuickJS provider WASM does not match its generated manifest")
  endif()
endif()

message(STATUS
  "QuickJS runtime profile: ${XAHAU_QUICKJS_RUNTIME_PROFILE_ID}")
