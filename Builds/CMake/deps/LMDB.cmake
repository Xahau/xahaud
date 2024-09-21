include(FetchContent)

FetchContent_Declare(
  lmdb
  GIT_REPOSITORY https://github.com/LMDB/lmdb.git
  GIT_TAG        LMDB_0.9.31
)

FetchContent_MakeAvailable(lmdb)

add_library(lmdb STATIC
  ${lmdb_SOURCE_DIR}/libraries/liblmdb/mdb.c
  ${lmdb_SOURCE_DIR}/libraries/liblmdb/midl.c
)

target_include_directories(lmdb PUBLIC ${lmdb_SOURCE_DIR}/libraries/liblmdb)

install(TARGETS lmdb
  EXPORT lmdbTargets
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
  INCLUDES DESTINATION include
)

install(DIRECTORY ${lmdb_SOURCE_DIR}/libraries/liblmdb/
  DESTINATION include
  FILES_MATCHING PATTERN "*.h"
)

add_library (NIH::lmdb ALIAS lmdb)
target_link_libraries (ripple_libs INTERFACE NIH::lmdb)