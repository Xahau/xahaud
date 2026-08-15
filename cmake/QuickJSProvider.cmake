set(XAHAU_QUICKJS_PROVIDER_BUNDLE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/external/quickjs-provider"
  CACHE PATH
  "Directory containing the sealed QuickJS provider WASM and generated manifests")
get_filename_component(XAHAU_QUICKJS_PROVIDER_BUNDLE_DIR
  "${XAHAU_QUICKJS_PROVIDER_BUNDLE_DIR}"
  ABSOLUTE
  BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(XAHAU_QUICKJS_PROVIDER_MANIFEST
  "${XAHAU_QUICKJS_PROVIDER_BUNDLE_DIR}/jshookz_provider.manifest.cmake")
set(XAHAU_QUICKJS_PROVIDER_PROFILE_LOCK
  "${XAHAU_QUICKJS_PROVIDER_BUNDLE_DIR}/jshookz_provider.manifest.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${XAHAU_QUICKJS_PROVIDER_PROFILE_LOCK}")

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

set(XAHAU_QUICKJS_NATIVE_ABI
  "${XAHAU_QUICKJS_PROVIDER_BUNDLE_DIR}/${XAHAU_QUICKJS_NATIVE_ABI_FILE}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${XAHAU_QUICKJS_NATIVE_ABI}"
  "${CMAKE_CURRENT_LIST_DIR}/GenerateQuickJSProviderPolicy.py")
if(NOT EXISTS "${XAHAU_QUICKJS_NATIVE_ABI}")
  message(FATAL_ERROR
    "Missing frozen QuickJS native ABI snapshot: ${XAHAU_QUICKJS_NATIVE_ABI}")
endif()
file(SHA256
  "${XAHAU_QUICKJS_NATIVE_ABI}"
  XAHAU_QUICKJS_ACTUAL_NATIVE_ABI_SHA256)
if(NOT XAHAU_QUICKJS_ACTUAL_NATIVE_ABI_SHA256 STREQUAL
    XAHAU_QUICKJS_NATIVE_ABI_SHA256)
  message(FATAL_ERROR
    "QuickJS native ABI snapshot does not match its sealed digest")
endif()

if(NOT DEFINED wasmtime_VERSION_STRING OR
   wasmtime_VERSION_STRING STREQUAL "")
  message(FATAL_ERROR
    "QuickJS provider requires an exact Wasmtime version, but the resolved "
    "package did not publish one")
elseif(NOT wasmtime_VERSION_STRING STREQUAL XAHAU_QUICKJS_WASMTIME_VERSION)
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
find_program(XAHAU_QUICKJS_PYTHON NAMES python3 python)
if(NOT XAHAU_QUICKJS_PYTHON)
  message(FATAL_ERROR
    "Python is required to project the sealed QuickJS provider policy")
endif()
execute_process(
  COMMAND
    "${XAHAU_QUICKJS_PYTHON}"
    "${CMAKE_CURRENT_LIST_DIR}/GenerateQuickJSProviderPolicy.py"
    --profile "${XAHAU_QUICKJS_PROVIDER_PROFILE_LOCK}"
    --native-abi "${XAHAU_QUICKJS_NATIVE_ABI}"
    --expected-count "${XAHAU_QUICKJS_PROVIDER_IMPORT_COUNT}"
    --output
      "${XAHAU_QUICKJS_GENERATED_INCLUDE_DIR}/xrpld/app/hook/detail/QuickJSProviderPolicy.inc"
  RESULT_VARIABLE XAHAU_QUICKJS_POLICY_RESULT
  ERROR_VARIABLE XAHAU_QUICKJS_POLICY_ERROR)
if(NOT XAHAU_QUICKJS_POLICY_RESULT EQUAL 0)
  message(FATAL_ERROR
    "Could not project the sealed QuickJS provider policy: "
    "${XAHAU_QUICKJS_POLICY_ERROR}")
endif()
configure_file(
  "${CMAKE_CURRENT_LIST_DIR}/QuickJSProviderProfile.h.in"
  "${XAHAU_QUICKJS_GENERATED_INCLUDE_DIR}/xrpld/app/hook/detail/QuickJSProviderProfile.h"
  @ONLY)

set(XAHAU_QUICKJS_PROVIDER_WASM
  "${XAHAU_QUICKJS_PROVIDER_BUNDLE_DIR}/${XAHAU_QUICKJS_PROVIDER_FILE}")
# The sealed provider is embedded into the binary when the bundle carries it,
# so the daemon can register it at startup (hook::embeddedQuickJSProvider).
# The generator re-verifies SHA-256 and size at build time; a bundle without
# the binary embeds nothing and the daemon says so at startup.
set(XAHAU_QUICKJS_PROVIDER_EMBED_SOURCE
  "${XAHAU_QUICKJS_GENERATED_INCLUDE_DIR}/xrpld/app/hook/detail/QuickJSProviderEmbed.cpp")
set(XAHAU_QUICKJS_EMBED_SCRIPT
  "${CMAKE_CURRENT_LIST_DIR}/EmbedQuickJSProvider.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${XAHAU_QUICKJS_PROVIDER_WASM}")
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
  # A stub written by an earlier bundle-less configure would be newer than
  # the wasm and let ninja skip the embed; drop it so the command must run.
  if(EXISTS "${XAHAU_QUICKJS_PROVIDER_EMBED_SOURCE}")
    file(STRINGS "${XAHAU_QUICKJS_PROVIDER_EMBED_SOURCE}"
      XAHAU_QUICKJS_EMBED_HEAD LIMIT_COUNT 2)
    if(XAHAU_QUICKJS_EMBED_HEAD MATCHES "no provider binary was present")
      file(REMOVE "${XAHAU_QUICKJS_PROVIDER_EMBED_SOURCE}")
    endif()
  endif()
  add_custom_command(
    OUTPUT "${XAHAU_QUICKJS_PROVIDER_EMBED_SOURCE}"
    COMMAND
      "${XAHAU_QUICKJS_PYTHON}" "${XAHAU_QUICKJS_EMBED_SCRIPT}"
      --provider "${XAHAU_QUICKJS_PROVIDER_WASM}"
      --sha256 "${XAHAU_QUICKJS_PROVIDER_SHA256}"
      --size "${XAHAU_QUICKJS_PROVIDER_SIZE}"
      --output "${XAHAU_QUICKJS_PROVIDER_EMBED_SOURCE}"
    DEPENDS
      "${XAHAU_QUICKJS_PROVIDER_WASM}"
      "${XAHAU_QUICKJS_EMBED_SCRIPT}"
      "${XAHAU_QUICKJS_PROVIDER_MANIFEST}"
    COMMENT "Embedding sealed QuickJS provider ${XAHAU_QUICKJS_PROVIDER_FILE}"
    VERBATIM)
  set(XAHAU_QUICKJS_PROVIDER_EMBEDDED ON)
else()
  execute_process(
    COMMAND
      "${XAHAU_QUICKJS_PYTHON}" "${XAHAU_QUICKJS_EMBED_SCRIPT}"
      --absent
      --output "${XAHAU_QUICKJS_PROVIDER_EMBED_SOURCE}"
    RESULT_VARIABLE XAHAU_QUICKJS_EMBED_RESULT
    ERROR_VARIABLE XAHAU_QUICKJS_EMBED_ERROR)
  if(NOT XAHAU_QUICKJS_EMBED_RESULT EQUAL 0)
    message(FATAL_ERROR
      "Could not project the absent QuickJS provider: "
      "${XAHAU_QUICKJS_EMBED_ERROR}")
  endif()
  set(XAHAU_QUICKJS_PROVIDER_EMBEDDED OFF)
  message(WARNING
    "No QuickJS provider WASM at ${XAHAU_QUICKJS_PROVIDER_WASM}; nothing is "
    "embedded and the daemon will not register a QuickJS runtime")
endif()

message(STATUS
  "QuickJS runtime profile: ${XAHAU_QUICKJS_RUNTIME_PROFILE_ID}")
message(STATUS
  "QuickJS provider embedded: ${XAHAU_QUICKJS_PROVIDER_EMBEDDED}")
