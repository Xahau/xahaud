#[===================================================================[
  Native LLVM source-based code coverage helper.

  Drives the -fprofile-instr-generate / -fcoverage-mapping pipeline:
    1. Run instrumented binary with LLVM_PROFILE_FILE=...%m-%p.profraw
    2. llvm-profdata merge -sparse  -> coverage.profdata
    3. llvm-cov export/show/report  -> final report

  Output filename per coverage_format:
    lcov              -> coverage.lcov
    json              -> coverage.json
    txt | text        -> coverage.txt
    html | html-details -> <NAME>/index.html
#]===================================================================]

include(CMakeParseArguments)

# Locate llvm-profdata / llvm-cov, preferring versioned variants matching the
# Clang we're building with so we don't accidentally pair clang-20 with
# llvm-cov-14 (profile format mismatch -> hard failure).
function(_find_llvm_cov_tools)
  if(LLVM_PROFDATA_PATH AND LLVM_COV_PATH)
    return()
  endif()

  string(REGEX MATCH "^[0-9]+" _major "${CMAKE_CXX_COMPILER_VERSION}")

  set(_pd_names llvm-profdata)
  set(_cov_names llvm-cov)
  if(_major)
    list(PREPEND _pd_names "llvm-profdata-${_major}")
    list(PREPEND _cov_names "llvm-cov-${_major}")
  endif()

  if(APPLE)
    execute_process(COMMAND xcrun -f llvm-profdata
      OUTPUT_VARIABLE _pd_xcrun OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET RESULT_VARIABLE _pd_rc)
    if(_pd_rc EQUAL 0 AND _pd_xcrun)
      set(LLVM_PROFDATA_PATH "${_pd_xcrun}" CACHE FILEPATH "llvm-profdata" FORCE)
    endif()
    execute_process(COMMAND xcrun -f llvm-cov
      OUTPUT_VARIABLE _cov_xcrun OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET RESULT_VARIABLE _cov_rc)
    if(_cov_rc EQUAL 0 AND _cov_xcrun)
      set(LLVM_COV_PATH "${_cov_xcrun}" CACHE FILEPATH "llvm-cov" FORCE)
    endif()
  endif()

  if(NOT LLVM_PROFDATA_PATH)
    find_program(LLVM_PROFDATA_PATH NAMES ${_pd_names})
  endif()
  if(NOT LLVM_COV_PATH)
    find_program(LLVM_COV_PATH NAMES ${_cov_names})
  endif()
endfunction()

function(setup_target_for_coverage_llvm)
  set(oneValueArgs NAME FORMAT BASE_DIRECTORY)
  set(multiValueArgs EXCLUDE EXECUTABLE EXECUTABLE_ARGS DEPENDENCIES)
  cmake_parse_arguments(Cov "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  _find_llvm_cov_tools()
  if(NOT LLVM_PROFDATA_PATH)
    message(FATAL_ERROR "llvm-profdata not found (needed for coverage_tool=llvm)")
  endif()
  if(NOT LLVM_COV_PATH)
    message(FATAL_ERROR "llvm-cov not found (needed for coverage_tool=llvm)")
  endif()

  if(NOT Cov_FORMAT)
    set(Cov_FORMAT lcov)
  endif()

  set(_profraw_dir "${PROJECT_BINARY_DIR}/${Cov_NAME}-profraw")
  set(_profdata    "${PROJECT_BINARY_DIR}/${Cov_NAME}.profdata")

  # Resolve binary path: accept either an absolute path or a bare target name
  # (resolved against PROJECT_BINARY_DIR).
  list(GET Cov_EXECUTABLE 0 _exec_name)
  if(IS_ABSOLUTE "${_exec_name}")
    set(_binary "${_exec_name}")
  else()
    set(_binary "${PROJECT_BINARY_DIR}/${_exec_name}")
  endif()

  # llvm-cov takes a single -ignore-filename-regex; OR our excludes together.
  set(_ignore_regex "")
  foreach(EXC IN LISTS Cov_EXCLUDE)
    if(_ignore_regex)
      string(APPEND _ignore_regex "|")
    endif()
    string(APPEND _ignore_regex "${EXC}")
  endforeach()
  set(_filter "")
  if(_ignore_regex)
    set(_filter "-ignore-filename-regex='${_ignore_regex}'")
  endif()

  # Pick llvm-cov subcommand + output file for the requested format. Each
  # branch builds a single shell command string that we'll hand to bash -c.
  if(Cov_FORMAT STREQUAL "lcov")
    set(_output "${PROJECT_BINARY_DIR}/coverage.lcov")
    set(_report_sh "${LLVM_COV_PATH} export -instr-profile='${_profdata}' -format=lcov ${_filter} '${_binary}' > '${_output}'")
  elseif(Cov_FORMAT STREQUAL "json")
    set(_output "${PROJECT_BINARY_DIR}/coverage.json")
    set(_report_sh "${LLVM_COV_PATH} export -instr-profile='${_profdata}' -format=text ${_filter} '${_binary}' > '${_output}'")
  elseif(Cov_FORMAT STREQUAL "txt" OR Cov_FORMAT STREQUAL "text")
    set(_output "${PROJECT_BINARY_DIR}/coverage.txt")
    set(_report_sh "${LLVM_COV_PATH} report -instr-profile='${_profdata}' ${_filter} '${_binary}' > '${_output}'")
  elseif(Cov_FORMAT STREQUAL "html" OR Cov_FORMAT STREQUAL "html-details")
    set(_output "${PROJECT_BINARY_DIR}/${Cov_NAME}/index.html")
    set(_report_sh "${LLVM_COV_PATH} show -instr-profile='${_profdata}' -format=html -output-dir='${PROJECT_BINARY_DIR}/${Cov_NAME}' ${_filter} '${_binary}'")
  else()
    message(FATAL_ERROR "coverage_tool=llvm: unsupported coverage_format '${Cov_FORMAT}' (use lcov|json|txt|html)")
  endif()

  set(_merge_sh "${LLVM_PROFDATA_PATH} merge -sparse -o '${_profdata}' '${_profraw_dir}'/*.profraw")

  if(CODE_COVERAGE_VERBOSE)
    message(STATUS "[coverage:llvm] binary:   ${_binary}")
    message(STATUS "[coverage:llvm] profraw:  ${_profraw_dir}")
    message(STATUS "[coverage:llvm] profdata: ${_profdata}")
    message(STATUS "[coverage:llvm] format:   ${Cov_FORMAT}")
    message(STATUS "[coverage:llvm] output:   ${_output}")
    if(_ignore_regex)
      message(STATUS "[coverage:llvm] ignore:   ${_ignore_regex}")
    endif()
    message(STATUS "[coverage:llvm] merge:    ${_merge_sh}")
    message(STATUS "[coverage:llvm] report:   ${_report_sh}")
  endif()

  # %m: hash of the binary, %p: pid. Wipe the dir up front so stale profraw
  # files can't leak into a fresh merge.
  add_custom_target(${Cov_NAME}
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${_profraw_dir}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_profraw_dir}"
    COMMAND ${CMAKE_COMMAND} -E env
        "LLVM_PROFILE_FILE=${_profraw_dir}/rippled-%m-%p.profraw"
        ${Cov_EXECUTABLE} ${Cov_EXECUTABLE_ARGS}
    COMMAND bash -c "${_merge_sh}"
    COMMAND bash -c "${_report_sh}"
    BYPRODUCTS ${_output}
    WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
    DEPENDS ${Cov_DEPENDENCIES}
    VERBATIM
    COMMENT "Running llvm-cov (${Cov_FORMAT}) -> ${_output}"
  )
endfunction()
