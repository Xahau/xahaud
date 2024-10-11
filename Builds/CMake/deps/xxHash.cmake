#[===================================================================[
 NIH dep: xxhash: 
#]===================================================================]


find_package(xxHash 0.8.2 CONFIG REQUIRED)
target_link_libraries(ripple_libs INTERFACE xxHash::xxhash)
add_library(NIH::xxhash ALIAS xxHash::xxhash)