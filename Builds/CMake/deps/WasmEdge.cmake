find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
add_library (wasmedge STATIC IMPORTED GLOBAL)
set_target_properties(wasmedge PROPERTIES IMPORTED_LOCATION ${WasmEdge_LIB})
target_link_libraries (ripple_libs INTERFACE wasmedge)
add_library (NIH::WasmEdge ALIAS wasmedge)
message("WasmEdge DONE")

