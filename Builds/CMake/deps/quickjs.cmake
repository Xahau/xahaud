# QuickJS source files
set(QUICKJS_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/quickjs/libutf.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/quickjs/qsort_r.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/quickjs/tutf8e.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/quickjs/vector.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/quickjs/char-utils.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/quickjs/buffer-utils.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/quickjs/utils.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/quickjs/ringbuffer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/quickjs/libregexp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/quickjs/quickjs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/quickjs/libregexp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/quickjs/libunicode.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/quickjs/cutils.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/quickjs/quickjs-libc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/quickjs/libbf.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/quickjs/quickjs-textcode.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/quickjs/quickjs-internal.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/quickjs/debug.c
)

# QuickJS include directories
set(QUICKJS_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src/quickjs)

# RHTODO: Fix this to be dynamic
set(QUICKJS_VERSION "2023-01-01")

# QuickJS compile definitions
set(QUICKJS_COMPILE_DEFINITIONS
    _GNU_SOURCE
    CONFIG_VERSION="${QUICKJS_VERSION}"
    CONFIG_BIGNUM
)

# Create static library
add_library(quickjs STATIC ${QUICKJS_SOURCES})
target_include_directories(quickjs
  PUBLIC
    $<BUILD_INTERFACE:${QUICKJS_INCLUDE_DIRS}>
    $<INSTALL_INTERFACE:include/quickjs>
)
target_compile_definitions(quickjs PUBLIC ${QUICKJS_COMPILE_DEFINITIONS})

add_library(NIH::quickjs ALIAS quickjs)
target_link_libraries(ripple_libs INTERFACE NIH::quickjs)

install(
    FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/quickjs/*.h
    DESTINATION include/quickjs
)
