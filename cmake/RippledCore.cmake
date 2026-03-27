#[===================================================================[
  Exported targets.
#]===================================================================]

include(target_protobuf_sources)

# Protocol buffers cannot participate in a unity build,
# because all the generated sources
# define a bunch of `static const` variables with the same names,
# so we just build them as a separate library.
add_library(xrpl.libpb)
set_target_properties(xrpl.libpb PROPERTIES UNITY_BUILD OFF)
target_protobuf_sources(xrpl.libpb xrpl/proto
  LANGUAGE cpp
  IMPORT_DIRS include/xrpl/proto
  PROTOS include/xrpl/proto/ripple.proto
)

file(GLOB_RECURSE protos "include/xrpl/proto/org/*.proto")
target_protobuf_sources(xrpl.libpb xrpl/proto
  LANGUAGE cpp
  IMPORT_DIRS include/xrpl/proto
  PROTOS "${protos}"
)
target_protobuf_sources(xrpl.libpb xrpl/proto
  LANGUAGE grpc
  IMPORT_DIRS include/xrpl/proto
  PROTOS "${protos}"
  PLUGIN protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
  GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
)

target_compile_options(xrpl.libpb
  PUBLIC
    $<$<BOOL:${MSVC}>:-wd4996>
    $<$<BOOL:${XCODE}>:
      --system-header-prefix="google/protobuf"
      -Wno-deprecated-dynamic-exception-spec
    >
  PRIVATE
    $<$<BOOL:${MSVC}>:-wd4065>
    $<$<NOT:$<BOOL:${MSVC}>>:-Wno-deprecated-declarations>
)

target_link_libraries(xrpl.libpb
  PUBLIC
    protobuf::libprotobuf
    gRPC::grpc++
)

# TODO: Clean up the number of library targets later.
add_library(xrpl.imports.main INTERFACE)

target_link_libraries(xrpl.imports.main
  INTERFACE
    LibArchive::LibArchive
    OpenSSL::Crypto
    Ripple::boost
    wasmedge::wasmedge
    Ripple::opts
    Ripple::syslibs
    absl::random_random
    date::date
    ed25519::ed25519
    secp256k1::secp256k1
    xrpl.libpb
    xxHash::xxhash
    $<$<BOOL:${voidstar}>:antithesis-sdk-cpp>
)

# date-tz for enhanced logging (always linked, code is #ifdef guarded)
if(TARGET date::date-tz)
  target_link_libraries(xrpl.imports.main INTERFACE date::date-tz)
endif()

# BEAST_ENHANCED_LOGGING: enable for Debug builds OR when explicitly requested
# Uses generator expression so it works with multi-config generators (Xcode, VS, Ninja Multi-Config)
target_compile_definitions(xrpl.imports.main INTERFACE
  $<$<OR:$<CONFIG:Debug>,$<BOOL:${BEAST_ENHANCED_LOGGING}>>:BEAST_ENHANCED_LOGGING=1>
)

include(add_module)
include(target_link_modules)

# Level 01
add_module(xrpl beast)
target_link_libraries(xrpl.libxrpl.beast PUBLIC
  xrpl.imports.main
  xrpl.libpb
)

# Level 02
add_module(xrpl basics)
target_link_libraries(xrpl.libxrpl.basics PUBLIC xrpl.libxrpl.beast)

# Level 03
add_module(xrpl json)
target_link_libraries(xrpl.libxrpl.json PUBLIC xrpl.libxrpl.basics)

add_module(xrpl crypto)
target_link_libraries(xrpl.libxrpl.crypto PUBLIC xrpl.libxrpl.basics)

add_module(xrpl hook)
target_link_libraries(xrpl.libxrpl.hook PUBLIC xrpl.libxrpl.basics)

# Level 04
add_module(xrpl protocol)
target_link_libraries(xrpl.libxrpl.protocol PUBLIC
  xrpl.libxrpl.crypto
  xrpl.libxrpl.hook
  xrpl.libxrpl.json
)

# Level 05
add_module(xrpl resource)
target_link_libraries(xrpl.libxrpl.resource PUBLIC xrpl.libxrpl.protocol)

add_module(xrpl server)
target_link_libraries(xrpl.libxrpl.server PUBLIC xrpl.libxrpl.protocol)


add_library(xrpl.libxrpl)
set_target_properties(xrpl.libxrpl PROPERTIES OUTPUT_NAME xrpl)

# Try to find the ACL library
find_library(ACL_LIBRARY NAMES acl)

# Check if ACL was found
if(ACL_LIBRARY)
message(STATUS "Found ACL: ${ACL_LIBRARY}")
else()
message(STATUS "ACL not found, continuing without ACL support")
endif()

add_library(xrpl::libxrpl ALIAS xrpl.libxrpl)

file(GLOB_RECURSE sources CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/libxrpl/*.cpp"
)
target_sources(xrpl.libxrpl PRIVATE ${sources})

target_link_modules(xrpl PUBLIC
  basics
  beast
  crypto
  hook
  json
  protocol
  resource
  server
)

# All headers in libxrpl are in modules.
# Uncomment this stanza if you have not yet moved new headers into a module.
# target_include_directories(xrpl.libxrpl
#   PRIVATE
#     $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
#   PUBLIC
#     $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
#     $<INSTALL_INTERFACE:include>)

if(xrpld)
  add_executable(rippled)
  if(tests)
    target_compile_definitions(rippled PUBLIC ENABLE_TESTS)
  endif()
  target_include_directories(rippled
    PRIVATE
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
  )

  file(GLOB_RECURSE sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/xrpld/*.cpp"
  )
  target_sources(rippled PRIVATE ${sources})

  if(tests)
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/src/test/*.cpp"
    )
    if(HOOKS_TEST_ONLY OR DEFINED ENV{HOOKS_TEST_ONLY})
      # Keep test infra but drop the individual *_test.cpp files
      list(FILTER sources EXCLUDE REGEX "_test\\.cpp$")
      message(STATUS "HOOKS_TEST_ONLY: excluded *_test.cpp from src/test/")
    endif()
    target_sources(rippled PRIVATE ${sources})

    # Optional: include external hook test sources from another directory.
    # Set via -DHOOKS_TEST_DIR=/path/to/tests or env HOOKS_TEST_DIR.
    # Optionally set HOOKS_C_DIR to pass --hooks-c-dir args to the compiler
    # (e.g. "tipbot=/path/to/hooks" — multiple values separated by ";").
    #
    # x-build-test-hooks must be on PATH. It auto-compiles hooks referenced
    # in each *_test.cpp and generates *_test_hooks.h next to the test file.
    if(NOT HOOKS_TEST_DIR AND DEFINED ENV{HOOKS_TEST_DIR})
      set(HOOKS_TEST_DIR $ENV{HOOKS_TEST_DIR})
    endif()
    if(NOT HOOKS_C_DIR AND DEFINED ENV{HOOKS_C_DIR})
      set(HOOKS_C_DIR $ENV{HOOKS_C_DIR})
    endif()
    if(HOOKS_TEST_DIR AND EXISTS "${HOOKS_TEST_DIR}")
      file(GLOB EXTERNAL_HOOK_TESTS CONFIGURE_DEPENDS
        "${HOOKS_TEST_DIR}/*_test.cpp"
      )
      if(EXTERNAL_HOOK_TESTS)
        # Build extra args for x-build-test-hooks
        set(_hooks_extra_args "")
        set(_hooks_source_deps "")
        if(HOOKS_C_DIR)
          foreach(_dir ${HOOKS_C_DIR})
            list(APPEND _hooks_extra_args "--hooks-c-dir" "${_dir}")

            string(REGEX REPLACE "^[^=]+=" "" _hook_dir "${_dir}")
            if(EXISTS "${_hook_dir}")
              file(GLOB_RECURSE _hook_dir_deps CONFIGURE_DEPENDS
                "${_hook_dir}/*.c"
                "${_hook_dir}/*.h"
              )
              if(HOOKS_TEST_DIR)
                list(FILTER _hook_dir_deps EXCLUDE REGEX "^${HOOKS_TEST_DIR}/")
              endif()
              list(APPEND _hooks_source_deps ${_hook_dir_deps})
            endif()
          endforeach()
          list(REMOVE_DUPLICATES _hooks_source_deps)
        endif()
        if(HOOKS_COVERAGE OR DEFINED ENV{HOOKS_COVERAGE})
          list(APPEND _hooks_extra_args "--hook-coverage")
          message(STATUS "Hook coverage enabled: compiling hooks with sancov")
        endif()

        # Run x-build-test-hooks on each test file before compilation
        foreach(_test_file ${EXTERNAL_HOOK_TESTS})
          get_filename_component(_stem ${_test_file} NAME_WE)
          set(_hooks_header "${HOOKS_TEST_DIR}/${_stem}_hooks.h")
          add_custom_command(
            OUTPUT "${_hooks_header}"
            COMMAND x-build-test-hooks "${_test_file}" ${_hooks_extra_args}
            DEPENDS "${_test_file}" ${_hooks_source_deps}
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            COMMENT "Compiling hooks for ${_stem}"
            VERBATIM
          )
          list(APPEND EXTERNAL_HOOK_HEADERS "${_hooks_header}")
        endforeach()

        # Ensure headers are generated before rippled compiles
        add_custom_target(compile_external_hooks DEPENDS ${EXTERNAL_HOOK_HEADERS})
        add_dependencies(rippled compile_external_hooks)

        target_sources(rippled PRIVATE ${EXTERNAL_HOOK_TESTS})
        target_include_directories(rippled PRIVATE "${HOOKS_TEST_DIR}")
        message(STATUS "Including external hook tests from: ${HOOKS_TEST_DIR}")
      endif()
    endif()
  endif()

  target_link_libraries(rippled
    Ripple::boost
    Ripple::opts
    Ripple::libs
    xrpl.libxrpl
    # Workaround for a Conan 1.x bug that prevents static linking of libstdc++
    # when a dependency (snappy) modifies system_libs. See the comment in
    # external/snappy/conanfile.py for a full explanation.
    # This is likely not strictly necessary, but listed explicitly as a good practice.
    m
  )
  exclude_if_included(rippled)
  # define a macro for tests that might need to
  # be exluded or run differently in CI environment
  if(is_ci)
    target_compile_definitions(rippled PRIVATE RIPPLED_RUNNING_IN_CI)
  endif ()

  if(voidstar)
    target_compile_options(rippled
      PRIVATE
        -fsanitize-coverage=trace-pc-guard
    )
    # rippled requires access to antithesis-sdk-cpp implementation file
    # antithesis_instrumentation.h, which is not exported as INTERFACE
    target_include_directories(rippled
      PRIVATE
        ${CMAKE_SOURCE_DIR}/external/antithesis-sdk
    )
  endif()

  # any files that don't play well with unity should be added here
  if(tests)
    set_source_files_properties(
      # these two seem to produce conflicts in beast teardown template methods
      src/test/rpc/ValidatorRPC_test.cpp
      src/test/ledger/Invariants_test.cpp
      PROPERTIES SKIP_UNITY_BUILD_INCLUSION TRUE)
  endif()
endif()

if(ACL_LIBRARY)
  target_link_libraries(rippled ${ACL_LIBRARY})
endif()
