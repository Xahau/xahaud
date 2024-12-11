include(FetchContent)

ExternalProject_Add(
  dilithium_src
  PREFIX ${nih_cache_path}
  GIT_REPOSITORY https://github.com/pq-crystals/dilithium.git
  GIT_TAG v3.1
  CMAKE_ARGS
    -DDILITHIUM_MODE=2 
    -DDILITHIUM_RANDOMIZED_SIGNING=ON 
    -DDILITHIUM_USE_AES=ON
  LOG_BUILD ON
  LOG_CONFIGURE ON
  COMMAND
    pwd
  BUILD_COMMAND
    ${CMAKE_COMMAND}
    --build .
    --config $<CONFIG>
    $<$<VERSION_GREATER_EQUAL:${CMAKE_VERSION},3.12>:--parallel ${ep_procs}>
  INSTALL_COMMAND ""
  BUILD_BYPRODUCTS
      <BINARY_DIR>/ref/libdilithium2_ref.a
      <BINARY_DIR>/ref/libdilithium2aes_ref.a
      <BINARY_DIR>/ref/libfips202_ref.a
)

ExternalProject_Get_Property(dilithium_src BINARY_DIR)
ExternalProject_Get_Property(dilithium_src SOURCE_DIR)
set(dilithium_src_SOURCE_DIR "${SOURCE_DIR}")
set(dilithium_src_BINARY_DIR "${BINARY_DIR}")

# Check if the api.h file exists
include_directories("${dilithium_src_SOURCE_DIR}/ref")

# Create imported targets for each static library
add_library(dilithium::dilithium2_ref STATIC IMPORTED GLOBAL)
set_target_properties(dilithium::dilithium2_ref PROPERTIES
  IMPORTED_LOCATION "${dilithium_src_BINARY_DIR}/ref/libdilithium2_ref.a"
  INTERFACE_INCLUDE_DIRECTORIES "${dilithium_src_SOURCE_DIR}/ref/"
)

add_library(dilithium::dilithium2aes_ref STATIC IMPORTED GLOBAL)
set_target_properties(dilithium::dilithium2aes_ref PROPERTIES
  IMPORTED_LOCATION "${dilithium_src_BINARY_DIR}/ref/libdilithium2aes_ref.a"
  INTERFACE_INCLUDE_DIRECTORIES "${dilithium_src_SOURCE_DIR}/ref/"
)

add_library(dilithium::libfips202_ref STATIC IMPORTED GLOBAL)
set_target_properties(dilithium::libfips202_ref PROPERTIES
  IMPORTED_LOCATION "${dilithium_src_BINARY_DIR}/ref/libfips202_ref.a"
  INTERFACE_INCLUDE_DIRECTORIES "${dilithium_src_SOURCE_DIR}/ref/"
)

add_dependencies(dilithium::dilithium2_ref dilithium_src)
add_dependencies(dilithium::dilithium2aes_ref dilithium_src)
add_dependencies(dilithium::libfips202_ref dilithium_src)

# Add a custom command to generate randombytes.c
add_custom_command(
  OUTPUT "${dilithium_src_BINARY_DIR}/ref/randombytes.c"
  COMMAND ${CMAKE_COMMAND} -E copy "${dilithium_src_SOURCE_DIR}/ref/randombytes.c" "${dilithium_src_BINARY_DIR}/ref/randombytes.c"
  DEPENDS dilithium_src
)

# Add the randombytes_ref library
add_library(randombytes_ref STATIC "${dilithium_src_BINARY_DIR}/ref/randombytes.c")

# Include the Dilithium ref directory for headers
target_include_directories(randombytes_ref PUBLIC "${dilithium_src_SOURCE_DIR}/ref")

# Ensure that randombytes_ref depends on the Dilithium source so it's built after
add_dependencies(randombytes_ref dilithium_src)

# Create an interface library that links to both
target_link_libraries(ripple_libs INTERFACE
  randombytes_ref
  dilithium::dilithium2_ref
  dilithium::dilithium2aes_ref
  dilithium::libfips202_ref
)

# Link the Dilithium library to your target
add_library(NIH::dilithium2_ref ALIAS dilithium::dilithium2_ref)