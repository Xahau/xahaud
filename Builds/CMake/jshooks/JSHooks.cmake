# JSHooks.cmake Generate JS hook headers for tests

# Find Python3
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Set paths
set(JSHOOK_SOURCE_DIR ${CMAKE_SOURCE_DIR}/src/test/app)
set(JSHOOK_BUILD_DIR ${CMAKE_BINARY_DIR}/jshooks/generated)
set(JSHOOK_CACHE_DIR ${CMAKE_BINARY_DIR}/jshooks/cache)
set(JSHOOK_SCRIPT_DIR ${CMAKE_CURRENT_LIST_DIR})

# Create directories
file(MAKE_DIRECTORY ${JSHOOK_BUILD_DIR})
file(MAKE_DIRECTORY ${JSHOOK_CACHE_DIR})

# Download and build qjsc if not provided
if(NOT DEFINED QJSC_BINARY OR QJSC_BINARY STREQUAL "")
  # Check if qjsc exists locally first
  set(LOCAL_QJSC_PATH ${JSHOOK_SOURCE_DIR}/qjsc)
  if(EXISTS ${LOCAL_QJSC_PATH})
    set(QJSC_BINARY ${LOCAL_QJSC_PATH})
  else()
    # Use FetchContent to download and build quickjslite
    include(FetchContent)
    
    FetchContent_Declare(
      quickjslite
      GIT_REPOSITORY https://github.com/RichardAH/quickjslite.git
      GIT_TAG        863bf70645dcf21e960af2f1540f40029d09498e
    )
    
    FetchContent_MakeAvailable(quickjslite)
    
    # Build qjsc using make
    set(QUICKJS_SOURCE_DIR ${quickjslite_SOURCE_DIR})
    set(QUICKJS_BINARY_DIR ${quickjslite_BINARY_DIR})
    set(QJSC_BINARY ${QUICKJS_SOURCE_DIR}/qjsc)
    
    # Custom target to build qjsc
    include(ProcessorCount)
    ProcessorCount(N)
    if(NOT N EQUAL 0)
      set(MAKE_JOBS -j${N})
    else()
      set(MAKE_JOBS "")
    endif()
    
    add_custom_command(
      OUTPUT ${QJSC_BINARY}
      COMMAND ${CMAKE_COMMAND} -E chdir ${QUICKJS_SOURCE_DIR} make ${MAKE_JOBS} qjsc
      DEPENDS ${QUICKJS_SOURCE_DIR}/Makefile
      COMMENT "Building qjsc from quickjslite (using ${N} jobs)..."
      VERBATIM
    )
    
    # Create a target for qjsc dependency
    add_custom_target(build_qjsc DEPENDS ${QJSC_BINARY})
  endif()
endif()

# Set up dependencies for qjsc
if(TARGET build_qjsc)
  set(QJSC_DEPENDENCY build_qjsc)
else()
  set(QJSC_DEPENDENCY "")
endif()

# Custom command to generate headers
add_custom_command(
  OUTPUT ${JSHOOK_SOURCE_DIR}/SetJSHook_wasm.h ${JSHOOK_BUILD_DIR}/.timestamp
  COMMAND
    ${CMAKE_COMMAND} -E env QJSC_BINARY=${QJSC_BINARY} ${Python3_EXECUTABLE}
    ${JSHOOK_SCRIPT_DIR}/build_test_jshooks.py --source
    ${JSHOOK_SOURCE_DIR}/SetJSHook_test.cpp --master
    ${JSHOOK_SOURCE_DIR}/SetJSHook_wasm.h --output-dir ${JSHOOK_BUILD_DIR}
    --cache-dir ${JSHOOK_CACHE_DIR} --log-level info
  COMMAND ${CMAKE_COMMAND} -E touch ${JSHOOK_BUILD_DIR}/.timestamp
  DEPENDS ${JSHOOK_SOURCE_DIR}/SetJSHook_test.cpp
          ${JSHOOK_SCRIPT_DIR}/build_test_jshooks.py
          ${QJSC_BINARY}
  WORKING_DIRECTORY ${JSHOOK_SOURCE_DIR}
  COMMENT "Generating JS hook headers..."
  VERBATIM)

# Custom target
add_custom_target(generate_js_hooks ALL
                  DEPENDS ${JSHOOK_SOURCE_DIR}/SetJSHook_wasm.h)

# Add qjsc dependency if needed
if(QJSC_DEPENDENCY)
  add_dependencies(generate_js_hooks ${QJSC_DEPENDENCY})
endif()

# Function to add JS hook support to a target
function(target_add_jshooks TARGET)
  target_include_directories(${TARGET} PRIVATE ${CMAKE_BINARY_DIR}/jshooks)
  add_dependencies(${TARGET} generate_js_hooks)
endfunction()

# Export variables for use by consumers
set(JSHOOK_GENERATED_DIR ${JSHOOK_BUILD_DIR})
set(JSHOOK_MASTER_HEADER ${JSHOOK_SOURCE_DIR}/SetJSHook_wasm.h)
