cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED QUICKJS_PROVIDER_RECEIPT OR
    NOT DEFINED QUICKJS_RECEIPT_TEST_TMP)
  message(FATAL_ERROR
    "Set QUICKJS_PROVIDER_RECEIPT and QUICKJS_RECEIPT_TEST_TMP")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/QuickJSProviderReceipt.cmake")

if(DEFINED QUICKJS_RECEIPT_CHILD)
  xahau_read_quickjs_provider_receipt(
    "${QUICKJS_PROVIDER_RECEIPT}" TEST_RECEIPT)
  return()
endif()

xahau_read_quickjs_provider_receipt(
  "${QUICKJS_PROVIDER_RECEIPT}" TEST_RECEIPT)
if(NOT TEST_RECEIPT_schema STREQUAL "jshookz.provider-receipt.v1" OR
    NOT TEST_RECEIPT_product STREQUAL "provider" OR
    NOT TEST_RECEIPT_provider_sha256 MATCHES "^[0-9a-f]+$")
  message(FATAL_ERROR "Current QuickJS provider receipt did not project")
endif()

file(READ "${QUICKJS_PROVIDER_RECEIPT}" _valid)
file(MAKE_DIRECTORY "${QUICKJS_RECEIPT_TEST_TMP}")

string(REGEX REPLACE "\n$" "" _missing_newline "${_valid}")
set(_unknown_key "${_valid}unknown_key value\n")
string(REPLACE "\n" "\n\n" _blank_line "${_valid}")
set(_duplicate_key "${_valid}schema jshookz.provider-receipt.v1\n")
set(_bad_space " bad value\n${_valid}")
set(_bad_tab "bad_key\tvalue\n${_valid}")

foreach(_case
    missing_newline
    unknown_key
    blank_line
    duplicate_key
    bad_space
    bad_tab)
  set(_mutant "${QUICKJS_RECEIPT_TEST_TMP}/${_case}.receipt")
  file(WRITE "${_mutant}" "${_${_case}}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
      -DQUICKJS_PROVIDER_RECEIPT=${_mutant}
      -DQUICKJS_RECEIPT_TEST_TMP=${QUICKJS_RECEIPT_TEST_TMP}
      -DQUICKJS_RECEIPT_CHILD=ON
      -P "${CMAKE_CURRENT_LIST_FILE}"
    RESULT_VARIABLE _result
    OUTPUT_QUIET
    ERROR_QUIET)
  if(_result EQUAL 0)
    message(FATAL_ERROR "QuickJS provider receipt mutant passed: ${_case}")
  endif()
endforeach()

message(STATUS "QuickJS provider receipt mutation checks passed")
