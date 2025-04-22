find_program(CLANG_TOOL clang PATHS ${LLVM_TOOLS_BINARY_DIR} NO_DEFAULT_PATH)
find_program(CLANGXX_TOOL clang++ PATHS ${LLVM_TOOLS_BINARY_DIR} NO_DEFAULT_PATH)
find_program(LLVMLINK_TOOL llvm-link PATHS ${LLVM_TOOLS_BINARY_DIR} NO_DEFAULT_PATH)
find_program(LLVMDIS_TOOL llvm-dis PATHS ${LLVM_TOOLS_BINARY_DIR} NO_DEFAULT_PATH)

add_subdirectory(micro_benchmark)
add_subdirectory(large_benchmark)