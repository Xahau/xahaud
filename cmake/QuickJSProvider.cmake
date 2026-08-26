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
set(XAHAU_QUICKJS_NATIVE_ABI
  "${XAHAU_QUICKJS_PROVIDER_BUNDLE_DIR}/jshookz_provider.native-abi.json")
set(XAHAU_QUICKJS_API_ARTIFACT_MANIFEST
  "${XAHAU_QUICKJS_PROVIDER_BUNDLE_DIR}/api-artifacts.json")
set(XAHAU_QUICKJS_BROAD_DECLARATION
  "${XAHAU_QUICKJS_PROVIDER_BUNDLE_DIR}/hooks-api.d.ts")
set(XAHAU_QUICKJS_EXACT_V1_DECLARATION
  "${XAHAU_QUICKJS_PROVIDER_BUNDLE_DIR}/xahau-quickjs-v1.d.ts")
set(XAHAU_QUICKJS_SELECTED_SURFACE
  "${XAHAU_QUICKJS_PROVIDER_BUNDLE_DIR}/xahau-quickjs-v1.surface.json")
set(XAHAU_QUICKJS_XFL_PROFILE_LEDGER
  "${XAHAU_QUICKJS_PROVIDER_BUNDLE_DIR}/xfl-profile-ledger.ts")

foreach(_bundle_file
    "${XAHAU_QUICKJS_PROVIDER_MANIFEST}"
    "${XAHAU_QUICKJS_PROVIDER_PROFILE_LOCK}"
    "${XAHAU_QUICKJS_NATIVE_ABI}"
    "${XAHAU_QUICKJS_API_ARTIFACT_MANIFEST}"
    "${XAHAU_QUICKJS_BROAD_DECLARATION}"
    "${XAHAU_QUICKJS_EXACT_V1_DECLARATION}"
    "${XAHAU_QUICKJS_SELECTED_SURFACE}"
    "${XAHAU_QUICKJS_XFL_PROFILE_LEDGER}")
  if(NOT EXISTS "${_bundle_file}")
    message(FATAL_ERROR
      "Missing sealed QuickJS provider bundle file: ${_bundle_file}")
  endif()
endforeach()

if(NOT DEFINED wasmtime_VERSION_STRING OR wasmtime_VERSION_STRING STREQUAL "")
  message(FATAL_ERROR
    "QuickJS provider requires an exact Wasmtime version, but the resolved "
    "package did not publish one")
endif()

find_program(XAHAU_QUICKJS_PYTHON NAMES python3 python)
if(NOT XAHAU_QUICKJS_PYTHON)
  message(FATAL_ERROR
    "Python is required to project the sealed QuickJS provider bundle")
endif()

set(XAHAU_QUICKJS_GENERATED_INCLUDE_DIR
  "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(XAHAU_QUICKJS_PROVIDER_VALUES_SOURCE
  "${XAHAU_QUICKJS_GENERATED_INCLUDE_DIR}/xrpld/app/hook/detail/QuickJSProviderValues.cpp")
set(XAHAU_QUICKJS_BUNDLE_SCRIPT
  "${CMAKE_CURRENT_LIST_DIR}/GenerateQuickJSProviderBundle.py")

# The gitignored wasm is not a CMake depend (ninja cannot name a missing
# file). Refresh on every rippled build; the script no-ops when inputs
# including wasm existence/digest are unchanged.
add_custom_target(xahau_quickjs_refresh_provider
  COMMAND
    "${XAHAU_QUICKJS_PYTHON}"
    "${XAHAU_QUICKJS_BUNDLE_SCRIPT}"
    --bundle-dir "${XAHAU_QUICKJS_PROVIDER_BUNDLE_DIR}"
    --wasmtime-version "${wasmtime_VERSION_STRING}"
    --output "${XAHAU_QUICKJS_PROVIDER_VALUES_SOURCE}"
  BYPRODUCTS "${XAHAU_QUICKJS_PROVIDER_VALUES_SOURCE}"
  DEPENDS
    "${XAHAU_QUICKJS_PROVIDER_PROFILE_LOCK}"
    "${XAHAU_QUICKJS_PROVIDER_MANIFEST}"
    "${XAHAU_QUICKJS_NATIVE_ABI}"
    "${XAHAU_QUICKJS_API_ARTIFACT_MANIFEST}"
    "${XAHAU_QUICKJS_BROAD_DECLARATION}"
    "${XAHAU_QUICKJS_EXACT_V1_DECLARATION}"
    "${XAHAU_QUICKJS_SELECTED_SURFACE}"
    "${XAHAU_QUICKJS_XFL_PROFILE_LEDGER}"
    "${XAHAU_QUICKJS_BUNDLE_SCRIPT}"
  COMMENT "Projecting sealed QuickJS provider values"
  VERBATIM)

message(STATUS
  "QuickJS provider values: ${XAHAU_QUICKJS_PROVIDER_VALUES_SOURCE}")

add_custom_target(xahau_quickjs_lock_mutations
  COMMAND
    "${XAHAU_QUICKJS_PYTHON}"
    "${CMAKE_CURRENT_LIST_DIR}/test_generate_quickjs_provider_bundle.py"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT "Proving sealed QuickJS lock mutation reds"
  VERBATIM)
