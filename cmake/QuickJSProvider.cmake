set(XAHAU_QUICKJS_PROVIDER_BUNDLE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/external/quickjs-provider"
  CACHE PATH
  "Directory containing the sealed QuickJS provider receipt and artifacts")
get_filename_component(XAHAU_QUICKJS_PROVIDER_BUNDLE_DIR
  "${XAHAU_QUICKJS_PROVIDER_BUNDLE_DIR}"
  ABSOLUTE
  BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

set(XAHAU_QUICKJS_PROVIDER_RECEIPT
  "${XAHAU_QUICKJS_PROVIDER_BUNDLE_DIR}/jshookz_provider.receipt")
include("${CMAKE_CURRENT_LIST_DIR}/QuickJSProviderReceipt.cmake")
xahau_read_quickjs_provider_receipt(
  "${XAHAU_QUICKJS_PROVIDER_RECEIPT}"
  XAHAU_QUICKJS_RECEIPT)

if(NOT XAHAU_QUICKJS_RECEIPT_schema STREQUAL
    "jshookz.provider-receipt.v1")
  message(FATAL_ERROR
    "Unsupported QuickJS provider receipt schema: "
    "${XAHAU_QUICKJS_RECEIPT_schema}")
endif()
if(NOT XAHAU_QUICKJS_RECEIPT_product STREQUAL "provider")
  message(FATAL_ERROR
    "QuickJS provider receipt names unsupported product: "
    "${XAHAU_QUICKJS_RECEIPT_product}")
endif()

foreach(_file_key
    api_artifacts_file
    manifest_file
    native_abi_file
    provider_file
    values_file)
  set(_file_name "${XAHAU_QUICKJS_RECEIPT_${_file_key}}")
  get_filename_component(_file_basename "${_file_name}" NAME)
  if(NOT _file_name STREQUAL _file_basename OR
      NOT _file_name MATCHES "^[A-Za-z0-9_.-]+$")
    message(FATAL_ERROR
      "QuickJS provider receipt ${_file_key} is not a safe basename")
  endif()
endforeach()

foreach(_sha_key
    api_artifacts_sha256
    broad_declaration_sha256
    bytecode_abi_id
    exact_v1_declaration_sha256
    manifest_sha256
    native_abi_sha256
    provider_sha256
    runtime_profile_id
    selected_surface_sha256
    values_sha256
    xfl_profile_ledger_sha256)
  set(_sha "${XAHAU_QUICKJS_RECEIPT_${_sha_key}}")
  string(LENGTH "${_sha}" _sha_length)
  if(NOT _sha_length EQUAL 64 OR NOT _sha MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR
      "QuickJS provider receipt ${_sha_key} is not a SHA-256 digest")
  endif()
endforeach()

foreach(_integer_key
    heap_bytes
    hook_api_version
    host_work_base_per_call
    host_work_budget
    host_work_per_addressed_byte
    initialization_fuel
    invocation_fuel
    provider_export_count
    provider_import_count
    provider_memory_maximum_pages
    provider_memory_minimum_pages
    provider_size
    serialized_object_max_bytes
    serialized_object_max_depth
    serialized_object_max_fields
    serialized_object_max_scopes
    stack_bytes
    wasm_stack_bytes)
  if(NOT XAHAU_QUICKJS_RECEIPT_${_integer_key} MATCHES "^[0-9]+$")
    message(FATAL_ERROR
      "QuickJS provider receipt ${_integer_key} is not an unsigned integer")
  endif()
endforeach()

if(NOT DEFINED wasmtime_VERSION_STRING OR wasmtime_VERSION_STRING STREQUAL "")
  message(FATAL_ERROR
    "QuickJS provider requires an exact Wasmtime version, but the resolved "
    "package did not publish one")
endif()
if(NOT wasmtime_VERSION_STRING STREQUAL
    XAHAU_QUICKJS_RECEIPT_wasmtime_version)
  message(FATAL_ERROR
    "QuickJS provider requires Wasmtime "
    "${XAHAU_QUICKJS_RECEIPT_wasmtime_version}, but the build resolved "
    "${wasmtime_VERSION_STRING}")
endif()

set(XAHAU_QUICKJS_PROVIDER_WASM
  "${XAHAU_QUICKJS_PROVIDER_BUNDLE_DIR}/${XAHAU_QUICKJS_RECEIPT_provider_file}")
set(XAHAU_QUICKJS_PROVIDER_VALUES_SOURCE
  "${XAHAU_QUICKJS_PROVIDER_BUNDLE_DIR}/${XAHAU_QUICKJS_RECEIPT_values_file}")
if(NOT EXISTS "${XAHAU_QUICKJS_PROVIDER_VALUES_SOURCE}")
  message(FATAL_ERROR
    "Missing preprojected QuickJS provider values: "
    "${XAHAU_QUICKJS_PROVIDER_VALUES_SOURCE}")
endif()
file(SHA256 "${XAHAU_QUICKJS_PROVIDER_VALUES_SOURCE}"
  _quickjs_values_sha256)
if(NOT _quickjs_values_sha256 STREQUAL
    XAHAU_QUICKJS_RECEIPT_values_sha256)
  message(FATAL_ERROR
    "Preprojected QuickJS provider values do not match the sealed receipt")
endif()

if(EXISTS "${XAHAU_QUICKJS_PROVIDER_WASM}")
  file(SHA256 "${XAHAU_QUICKJS_PROVIDER_WASM}" _quickjs_wasm_sha256)
  file(SIZE "${XAHAU_QUICKJS_PROVIDER_WASM}" _quickjs_wasm_size)
  if(NOT _quickjs_wasm_sha256 STREQUAL
      XAHAU_QUICKJS_RECEIPT_provider_sha256 OR
      NOT "${_quickjs_wasm_size}" STREQUAL
      "${XAHAU_QUICKJS_RECEIPT_provider_size}")
    message(FATAL_ERROR
      "QuickJS provider WASM does not match the sealed receipt")
  endif()
endif()

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${XAHAU_QUICKJS_PROVIDER_RECEIPT}"
  "${XAHAU_QUICKJS_PROVIDER_VALUES_SOURCE}")

set(XAHAU_QUICKJS_GENERATED_INCLUDE_DIR
  "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(XAHAU_QUICKJS_PROVIDER_EMBED_SOURCE
  "${XAHAU_QUICKJS_GENERATED_INCLUDE_DIR}/xrpld/app/hook/detail/QuickJSProviderEmbed.cpp")
set(XAHAU_QUICKJS_EMBED_SCRIPT
  "${CMAKE_CURRENT_LIST_DIR}/EmbedQuickJSProvider.cmake")
set_source_files_properties(
  "${XAHAU_QUICKJS_PROVIDER_EMBED_SOURCE}" PROPERTIES GENERATED TRUE)

# The gitignored wasm cannot be a Ninja dependency when it is absent. This
# target therefore runs every rippled build; the CMake script verifies both
# pinned artifacts and no-ops when its stamp is current.
add_custom_target(xahau_quickjs_refresh_provider
  COMMAND "${CMAKE_COMMAND}"
    "-DINPUT=${XAHAU_QUICKJS_PROVIDER_WASM}"
    "-DVALUES_INPUT=${XAHAU_QUICKJS_PROVIDER_VALUES_SOURCE}"
    "-DEXPECTED_SHA256=${XAHAU_QUICKJS_RECEIPT_provider_sha256}"
    "-DEXPECTED_SIZE=${XAHAU_QUICKJS_RECEIPT_provider_size}"
    "-DEXPECTED_VALUES_SHA256=${XAHAU_QUICKJS_RECEIPT_values_sha256}"
    "-DOUTPUT=${XAHAU_QUICKJS_PROVIDER_EMBED_SOURCE}"
    -P "${XAHAU_QUICKJS_EMBED_SCRIPT}"
  BYPRODUCTS "${XAHAU_QUICKJS_PROVIDER_EMBED_SOURCE}"
  DEPENDS
    "${XAHAU_QUICKJS_PROVIDER_RECEIPT}"
    "${XAHAU_QUICKJS_PROVIDER_VALUES_SOURCE}"
    "${XAHAU_QUICKJS_EMBED_SCRIPT}"
  COMMENT "Verifying and embedding the sealed QuickJS provider"
  VERBATIM)

message(STATUS
  "QuickJS provider receipt: ${XAHAU_QUICKJS_PROVIDER_RECEIPT}")
message(STATUS
  "QuickJS provider values: ${XAHAU_QUICKJS_PROVIDER_VALUES_SOURCE}")

add_custom_target(xahau_quickjs_receipt_mutations
  COMMAND "${CMAKE_COMMAND}"
    "-DQUICKJS_PROVIDER_RECEIPT=${XAHAU_QUICKJS_PROVIDER_RECEIPT}"
    "-DQUICKJS_RECEIPT_TEST_TMP=${CMAKE_CURRENT_BINARY_DIR}/quickjs-receipt-tests"
    -P "${CMAKE_CURRENT_LIST_DIR}/TestQuickJSProviderReceipt.cmake"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT "Proving sealed QuickJS receipt mutation reds"
  VERBATIM)
