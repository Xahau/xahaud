include_guard(GLOBAL)

function(xahaud_require_lean_toolchain project_dir)
  if(NOT EXISTS "${project_dir}/lean-toolchain")
    message(FATAL_ERROR "Lean project is missing lean-toolchain: ${project_dir}")
  endif()

  file(READ "${project_dir}/lean-toolchain" lean_toolchain)
  string(STRIP "${lean_toolchain}" lean_toolchain)
  if(NOT lean_toolchain MATCHES "^leanprover/lean4:v(.+)$")
    message(FATAL_ERROR
      "Unsupported lean-toolchain format `${lean_toolchain}` in ${project_dir}")
  endif()
  set(expected_lean_version "${CMAKE_MATCH_1}")

  find_program(LAKE_EXECUTABLE
    NAMES lake
    HINTS "$ENV{HOME}/.elan/bin")
  if(NOT LAKE_EXECUTABLE)
    message(FATAL_ERROR
      "formal_verification=ON requires Lake on PATH or in ~/.elan/bin. "
      "Install elan, then run `lake build` once in ${project_dir}.")
  endif()

  find_program(LEAN_EXECUTABLE
    NAMES lean
    HINTS "$ENV{HOME}/.elan/bin")
  if(NOT LEAN_EXECUTABLE)
    message(FATAL_ERROR
      "formal_verification=ON requires Lean on PATH or in ~/.elan/bin")
  endif()

  execute_process(
    COMMAND "${LEAN_EXECUTABLE}" --version
    WORKING_DIRECTORY "${project_dir}"
    OUTPUT_VARIABLE lean_version_output
    ERROR_VARIABLE lean_version_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE lean_version_result)
  if(NOT lean_version_result EQUAL 0)
    message(FATAL_ERROR
      "Could not run `${LEAN_EXECUTABLE} --version`: ${lean_version_error}")
  endif()
  if(NOT lean_version_output MATCHES "Lean \\(version ${expected_lean_version}[,)]")
    message(FATAL_ERROR
      "Lean version mismatch for formal_verification=ON. "
      "Expected ${expected_lean_version} from ${project_dir}/lean-toolchain, "
      "but `${LEAN_EXECUTABLE} --version` returned `${lean_version_output}`")
  endif()

  execute_process(
    COMMAND "${LAKE_EXECUTABLE}" --version
    WORKING_DIRECTORY "${project_dir}"
    OUTPUT_VARIABLE lake_version_output
    ERROR_VARIABLE lake_version_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE lake_version_result)
  if(NOT lake_version_result EQUAL 0)
    message(FATAL_ERROR
      "Could not run `${LAKE_EXECUTABLE} --version`: ${lake_version_error}")
  endif()
  if(NOT lake_version_output MATCHES "Lean version ${expected_lean_version}[)]")
    message(FATAL_ERROR
      "Lake version mismatch for formal_verification=ON. "
      "Expected Lean ${expected_lean_version} from ${project_dir}/lean-toolchain, "
      "but `${LAKE_EXECUTABLE} --version` returned `${lake_version_output}`")
  endif()

  if(NOT EXISTS "${project_dir}/lakefile.toml")
    message(FATAL_ERROR
      "formal_verification=ON requires ${project_dir}/lakefile.toml")
  endif()

  execute_process(
    COMMAND "${LAKE_EXECUTABLE}" env printenv LEAN_SYSROOT
    WORKING_DIRECTORY "${project_dir}"
    OUTPUT_VARIABLE lean_sysroot
    ERROR_VARIABLE lean_sysroot_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE lean_sysroot_result)
  if(NOT lean_sysroot_result EQUAL 0 OR NOT lean_sysroot)
    message(FATAL_ERROR
      "Could not determine Lean sysroot via "
      "`${LAKE_EXECUTABLE} env printenv LEAN_SYSROOT`: ${lean_sysroot_error}")
  endif()

  set(lean_include_dir "${lean_sysroot}/include")
  if(NOT EXISTS "${lean_include_dir}/lean/lean.h")
    message(FATAL_ERROR "Lean header not found: ${lean_include_dir}/lean/lean.h")
  endif()

  find_library(lean_shared_library
    NAMES leanshared libleanshared
    PATHS "${lean_sysroot}/lib/lean"
    NO_DEFAULT_PATH)
  if(NOT lean_shared_library)
    message(FATAL_ERROR
      "Lean shared runtime not found under ${lean_sysroot}/lib/lean")
  endif()

  set(LAKE_EXECUTABLE "${LAKE_EXECUTABLE}" PARENT_SCOPE)
  set(LEAN_EXECUTABLE "${LEAN_EXECUTABLE}" PARENT_SCOPE)
  set(LEAN_SYSROOT "${lean_sysroot}" PARENT_SCOPE)
  set(LEAN_INCLUDE_DIR "${lean_include_dir}" PARENT_SCOPE)
  set(LEAN_SHARED_LIBRARY "${lean_shared_library}" PARENT_SCOPE)
  set(LEAN_EXPECTED_VERSION "${expected_lean_version}" PARENT_SCOPE)
endfunction()
