#[===================================================================[
 NIH dep: mongo: MongoDB C++ driver (bsoncxx and mongocxx).
#]===================================================================]

include(FetchContent)

FetchContent_Declare(
  mongo_c_driver_src
  GIT_REPOSITORY https://github.com/mongodb/mongo-c-driver.git
  GIT_TAG        1.17.4
)

FetchContent_GetProperties(mongo_c_driver_src)
if(NOT mongo_c_driver_src_POPULATED)
  message(STATUS "Pausing to download MongoDB C driver...")
  FetchContent_Populate(mongo_c_driver_src)
endif()

set(MONGO_C_DRIVER_BUILD_DIR "${mongo_c_driver_src_BINARY_DIR}")
set(MONGO_C_DRIVER_INCLUDE_DIR "${mongo_c_driver_src_SOURCE_DIR}/src/libbson/src")
set(MONGO_C_DRIVER_INSTALL_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/mongo_c_install")

set(MONGO_C_DRIVER_CMAKE_ARGS
  -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
  -DENABLE_AUTOMATIC_INIT_AND_CLEANUP=OFF
  -DENABLE_STATIC=ON
  -DENABLE_SHARED=OFF
  -DCMAKE_INSTALL_PREFIX=${MONGO_C_DRIVER_INSTALL_PREFIX}
)

ExternalProject_Add(mongo_c_driver
  PREFIX ${CMAKE_CURRENT_BINARY_DIR}/mongo_c
  SOURCE_DIR ${mongo_c_driver_src_SOURCE_DIR}
  CMAKE_ARGS ${MONGO_C_DRIVER_CMAKE_ARGS}
  BUILD_COMMAND ${CMAKE_COMMAND} --build . --config $<CONFIG>
  INSTALL_COMMAND ${CMAKE_COMMAND} --install .
)

FetchContent_Declare(
  mongo_cxx_driver_src
  GIT_REPOSITORY https://github.com/mongodb/mongo-cxx-driver.git
  GIT_TAG        r3.10.2
)

FetchContent_GetProperties(mongo_cxx_driver_src)
if(NOT mongo_cxx_driver_src_POPULATED)
  message(STATUS "Pausing to download MongoDB C++ driver...")
  FetchContent_Populate(mongo_cxx_driver_src)
endif()

set(MONGO_CXX_DRIVER_BUILD_DIR "${mongo_cxx_driver_src_BINARY_DIR}")
set(MONGO_CXX_DRIVER_INCLUDE_DIR "${mongo_cxx_driver_src_SOURCE_DIR}/include")
set(MONGO_CXX_DRIVER_INSTALL_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/mongo_cxx_install")

set(MONGO_CXX_DRIVER_CMAKE_ARGS
  -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
  -DBUILD_SHARED_AND_STATIC_LIBS=ON
  -DBSONCXX_ENABLE_MONGOC=ON
  -DCMAKE_INSTALL_PREFIX=${MONGO_CXX_DRIVER_INSTALL_PREFIX}
  -DCMAKE_PREFIX_PATH=${MONGO_C_DRIVER_INSTALL_PREFIX}
)

ExternalProject_Add(mongo_cxx_driver
  PREFIX ${CMAKE_CURRENT_BINARY_DIR}/mongo_cxx
  SOURCE_DIR ${mongo_cxx_driver_src_SOURCE_DIR}
  CMAKE_ARGS ${MONGO_CXX_DRIVER_CMAKE_ARGS}
  BUILD_COMMAND ${CMAKE_COMMAND} --build . --config $<CONFIG>
  INSTALL_COMMAND ${CMAKE_COMMAND} --install .
  DEPENDS mongo_c_driver
)

add_library(bsoncxx STATIC IMPORTED GLOBAL)
add_library(mongocxx STATIC IMPORTED GLOBAL)
add_dependencies(bsoncxx mongo_cxx_driver)
add_dependencies(mongocxx mongo_cxx_driver)

ExternalProject_Get_Property(mongo_cxx_driver BINARY_DIR)
execute_process(
    COMMAND
        mkdir -p "${BINARY_DIR}/include/bsoncxx/v_noabi"
        mkdir -p "${BINARY_DIR}/include/mongocxx/v_noabi"
)

set_target_properties(bsoncxx PROPERTIES
  IMPORTED_LOCATION "${MONGO_CXX_DRIVER_INSTALL_PREFIX}/lib/libbsoncxx-static.a"
  INTERFACE_INCLUDE_DIRECTORIES "${MONGO_CXX_DRIVER_INSTALL_PREFIX}/include/bsoncxx/v_noabi"
)

set_target_properties(mongocxx PROPERTIES
  IMPORTED_LOCATION "${MONGO_CXX_DRIVER_INSTALL_PREFIX}/lib/libmongocxx-static.a"
  INTERFACE_INCLUDE_DIRECTORIES "${MONGO_CXX_DRIVER_INSTALL_PREFIX}/include/mongocxx/v_noabi"
)

# Link the C driver libraries
find_library(BSON_LIB bson-1.0 PATHS ${MONGO_C_DRIVER_INSTALL_PREFIX}/lib)
find_library(MONGOC_LIB mongoc-1.0 PATHS ${MONGO_C_DRIVER_INSTALL_PREFIX}/lib)

target_link_libraries(ripple_libs INTERFACE bsoncxx mongocxx ${BSON_LIB} ${MONGOC_LIB})

add_library(NIH::MongoCxx ALIAS mongocxx)