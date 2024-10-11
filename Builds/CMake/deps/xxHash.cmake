#[===================================================================[
 NIH dep: xxhash: 
#]===================================================================]


# find_package(xxHash 0.8.2 CONFIG REQUIRED)
# target_link_libraries(ripple_libs INTERFACE xxHash::xxhash)
# add_library(NIH::xxhash ALIAS xxHash::xxhash)

include(FetchContent)

ExternalProject_Add(
  xxhash_src
  PREFIX ${nih_cache_path}
  GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
  GIT_TAG v0.8.2
  SOURCE_SUBDIR cmake_unofficial
  CMAKE_ARGS 
   -DBUILD_SHARED_LIBS=OFF 
   -DXXHASH_BUILD_XXHSUM=ON
   LOG_BUILD ON
   LOG_CONFIGURE ON
   INSTALL_COMMAND ""
   BUILD_BYPRODUCTS
      <BINARY_DIR>/libxxhash.a
)
add_library(xxHash::xxhash STATIC IMPORTED GLOBAL)

ExternalProject_Get_Property (xxhash_src BINARY_DIR)
ExternalProject_Get_Property (xxhash_src SOURCE_DIR)
set (xxhash_src_SOURCE_DIR "${SOURCE_DIR}")
set (xxhash_src_BINARY_DIR "${BINARY_DIR}")

add_dependencies(xxHash::xxhash xxhash_src)
set_target_properties (xxHash::xxhash PROPERTIES
  IMPORTED_LOCATION_DEBUG
    "${xxhash_src_BINARY_DIR}/libxxhash.a"
  IMPORTED_LOCATION_RELEASE
    "${xxhash_src_BINARY_DIR}/libxxhash.a"
  INTERFACE_INCLUDE_DIRECTORIES
    "${xxhash_src_SOURCE_DIR}"
)

target_link_libraries(ripple_libs INTERFACE xxHash::xxhash)
add_library(NIH::xxhash ALIAS xxHash::xxhash)