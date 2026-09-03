# Parse the producer's non-executable, CMake-3.16-compatible receipt.

function(xahau_read_quickjs_provider_receipt receipt prefix)
  if(NOT EXISTS "${receipt}")
    message(FATAL_ERROR "Missing sealed QuickJS provider receipt: ${receipt}")
  endif()

  file(READ "${receipt}" _receipt_text)
  if(_receipt_text STREQUAL "" OR NOT "${_receipt_text}" MATCHES "\n$")
    message(FATAL_ERROR
      "QuickJS provider receipt must be non-empty and newline-terminated")
  endif()
  foreach(_forbidden "\r" "\t" ";" "#" "\n\n")
    string(FIND "${_receipt_text}" "${_forbidden}" _forbidden_at)
    if(NOT _forbidden_at EQUAL -1)
      message(FATAL_ERROR
        "QuickJS provider receipt contains forbidden whitespace or syntax")
    endif()
  endforeach()

  set(_expected_keys
    api_artifacts_file
    api_artifacts_sha256
    broad_declaration_sha256
    bytecode_abi_id
    exact_v1_declaration_sha256
    heap_bytes
    hook_api_version
    host_adapter_policy
    host_work_base_per_call
    host_work_budget
    host_work_meter
    host_work_per_addressed_byte
    initialization_fuel
    invocation_fuel
    manifest_file
    manifest_schema
    manifest_sha256
    native_abi_file
    native_abi_sha256
    product
    provider_export_count
    provider_file
    provider_import_count
    provider_memory_maximum_pages
    provider_memory_minimum_pages
    provider_sha256
    provider_size
    runtime_profile_id
    schema
    selected_surface_sha256
    serialized_object_max_bytes
    serialized_object_max_depth
    serialized_object_max_fields
    serialized_object_max_scopes
    stack_bytes
    values_file
    values_sha256
    wasm_stack_bytes
    wasmtime_version
    xfl_profile_ledger_sha256)

  file(STRINGS "${receipt}" _receipt_lines ENCODING UTF-8)
  set(_actual_keys)
  foreach(_line IN LISTS _receipt_lines)
    if(NOT "${_line}" MATCHES "^([a-z0-9_]+) ([!-~]+)$")
      message(FATAL_ERROR
        "QuickJS provider receipt line is not exactly 'key value': ${_line}")
    endif()
    set(_key "${CMAKE_MATCH_1}")
    set(_value "${CMAKE_MATCH_2}")
    if(DEFINED _receipt_value_${_key})
      message(FATAL_ERROR "QuickJS provider receipt repeats key: ${_key}")
    endif()
    set(_receipt_value_${_key} "${_value}")
    list(APPEND _actual_keys "${_key}")
  endforeach()

  if(NOT "${_actual_keys}" STREQUAL "${_expected_keys}")
    message(FATAL_ERROR
      "QuickJS provider receipt keys are missing, extra, or not sorted")
  endif()

  foreach(_key IN LISTS _expected_keys)
    set("${prefix}_${_key}" "${_receipt_value_${_key}}" PARENT_SCOPE)
  endforeach()
endfunction()
