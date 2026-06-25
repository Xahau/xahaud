if(NOT formal_verification)
  return()
endif()

if(NOT xrpld)
  message(FATAL_ERROR "formal_verification requires xrpld=ON")
endif()

if(NOT tests)
  message(FATAL_ERROR "formal_verification requires tests=ON")
endif()

if(CMAKE_CROSSCOMPILING)
  message(FATAL_ERROR "formal_verification currently supports native builds only")
endif()

if(WIN32)
  message(FATAL_ERROR "formal_verification currently supports Unix-like native builds only")
endif()

set(XAHAU_FORMAL_VERIFICATION_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/formal_verification"
  CACHE PATH
  "Lean formal-verification project used by formal_verification=ON")

include(XahaudLean)
xahaud_require_lean_toolchain("${XAHAU_FORMAL_VERIFICATION_DIR}")

set(XAHAU_FORMAL_ARCHIVE
  "${XAHAU_FORMAL_VERIFICATION_DIR}/.lake/build/lib/libxahau__consensus_XahauConsensus.a")

file(GLOB_RECURSE XAHAU_FORMAL_SOURCES CONFIGURE_DEPENDS
  "${XAHAU_FORMAL_VERIFICATION_DIR}/*.lean")

# Lake currently writes package artifacts under the Lean workspace's .lake/
# directory. Keep this option native/test-only until the build is moved to a
# copied CMake-binary-dir workspace or Lake grows a stable external build-dir
# interface we can rely on here.
#
# This target deliberately invokes Lake whenever the formal-enabled `rippled`
# target is built. Lake still performs its own incremental rebuild, but CMake
# must not trust a source-tree `.lake` archive purely by timestamp.
add_custom_target(xahaud_formal_verification_lean
  COMMAND "${LAKE_EXECUTABLE}" build XahauConsensus:static
  WORKING_DIRECTORY "${XAHAU_FORMAL_VERIFICATION_DIR}"
  DEPENDS
    "${XAHAU_FORMAL_VERIFICATION_DIR}/lakefile.toml"
    "${XAHAU_FORMAL_VERIFICATION_DIR}/lean-toolchain"
    "${XAHAU_FORMAL_VERIFICATION_DIR}/lake-manifest.json"
    ${XAHAU_FORMAL_SOURCES}
  BYPRODUCTS "${XAHAU_FORMAL_ARCHIVE}"
  COMMENT "Building Lean formal-verification archive"
  VERBATIM)

add_dependencies(rippled xahaud_formal_verification_lean)
target_compile_definitions(rippled PRIVATE XAHAUD_ENABLE_FORMAL_VERIFICATION=1)
target_include_directories(rippled PRIVATE "${LEAN_INCLUDE_DIR}")
target_link_libraries(rippled "${XAHAU_FORMAL_ARCHIVE}" "${LEAN_SHARED_LIBRARY}")

if(UNIX)
  set_property(TARGET rippled APPEND PROPERTY BUILD_RPATH "${LEAN_SYSROOT}/lib/lean")
endif()

message(STATUS "Formal verification enabled: ${XAHAU_FORMAL_VERIFICATION_DIR}")
message(STATUS "Lean ${LEAN_EXPECTED_VERSION} sysroot: ${LEAN_SYSROOT}")
