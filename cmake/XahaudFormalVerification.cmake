if(NOT formal_verification)
  return()
endif()

if(NOT xrpld)
  message(FATAL_ERROR "formal_verification requires xrpld=ON")
endif()

if(NOT tests)
  message(FATAL_ERROR "formal_verification requires tests=ON")
endif()

set(XAHAU_FORMAL_VERIFICATION_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/formal_verification"
  CACHE PATH
  "Lean formal-verification project used by formal_verification=ON")

find_program(LAKE_EXECUTABLE
  NAMES lake
  HINTS "$ENV{HOME}/.elan/bin")

if(NOT LAKE_EXECUTABLE)
  message(FATAL_ERROR "formal_verification=ON requires Lake on PATH or in ~/.elan/bin")
endif()

if(NOT EXISTS "${XAHAU_FORMAL_VERIFICATION_DIR}/lakefile.toml")
  message(FATAL_ERROR
    "formal_verification=ON requires ${XAHAU_FORMAL_VERIFICATION_DIR}/lakefile.toml")
endif()

execute_process(
  COMMAND ${LAKE_EXECUTABLE} env printenv LEAN_SYSROOT
  WORKING_DIRECTORY "${XAHAU_FORMAL_VERIFICATION_DIR}"
  OUTPUT_VARIABLE LEAN_SYSROOT
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE LEAN_SYSROOT_RESULT)

if(NOT LEAN_SYSROOT_RESULT EQUAL 0 OR NOT LEAN_SYSROOT)
  message(FATAL_ERROR "Could not determine Lean sysroot via `${LAKE_EXECUTABLE} env printenv LEAN_SYSROOT`")
endif()

set(LEAN_INCLUDE_DIR "${LEAN_SYSROOT}/include")
if(NOT EXISTS "${LEAN_INCLUDE_DIR}/lean/lean.h")
  message(FATAL_ERROR "Lean header not found: ${LEAN_INCLUDE_DIR}/lean/lean.h")
endif()

find_library(LEAN_SHARED_LIBRARY
  NAMES leanshared libleanshared
  PATHS "${LEAN_SYSROOT}/lib/lean"
  NO_DEFAULT_PATH)

if(NOT LEAN_SHARED_LIBRARY)
  message(FATAL_ERROR "Lean shared runtime not found under ${LEAN_SYSROOT}/lib/lean")
endif()

set(XAHAU_FORMAL_ARCHIVE
  "${XAHAU_FORMAL_VERIFICATION_DIR}/.lake/build/lib/libxahau__consensus_XahauConsensus.a")

file(GLOB_RECURSE XAHAU_FORMAL_SOURCES CONFIGURE_DEPENDS
  "${XAHAU_FORMAL_VERIFICATION_DIR}/*.lean")

add_custom_command(
  OUTPUT "${XAHAU_FORMAL_ARCHIVE}"
  COMMAND ${LAKE_EXECUTABLE} build XahauConsensus:static
  WORKING_DIRECTORY "${XAHAU_FORMAL_VERIFICATION_DIR}"
  DEPENDS
    "${XAHAU_FORMAL_VERIFICATION_DIR}/lakefile.toml"
    "${XAHAU_FORMAL_VERIFICATION_DIR}/lean-toolchain"
    "${XAHAU_FORMAL_VERIFICATION_DIR}/lake-manifest.json"
    ${XAHAU_FORMAL_SOURCES}
  COMMENT "Building Lean formal-verification archive"
  VERBATIM)

add_custom_target(xahaud_formal_verification_lean
  DEPENDS "${XAHAU_FORMAL_ARCHIVE}")

add_dependencies(rippled xahaud_formal_verification_lean)
target_compile_definitions(rippled PRIVATE XAHAUD_ENABLE_FORMAL_VERIFICATION=1)
target_include_directories(rippled PRIVATE "${LEAN_INCLUDE_DIR}")
target_link_libraries(rippled "${XAHAU_FORMAL_ARCHIVE}" "${LEAN_SHARED_LIBRARY}")

if(APPLE)
  set_property(TARGET rippled APPEND PROPERTY BUILD_RPATH "${LEAN_SYSROOT}/lib/lean")
endif()

message(STATUS "Formal verification enabled: ${XAHAU_FORMAL_VERIFICATION_DIR}")
message(STATUS "Lean sysroot: ${LEAN_SYSROOT}")
