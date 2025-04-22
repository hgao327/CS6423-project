cmake_minimum_required(VERSION 3.18)
project(cs6245-proj1)

set(CMAKE_CXX_STANDARD 14)
find_package(LLVM 15 REQUIRED CONFIG)
add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})
link_directories(${LLVM_LIBRARY_DIRS})

if(NOT LLVM_ENABLE_RTTI)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")
endif()

add_subdirectory(src)
add_subdirectory(script)
add_subdirectory(benchmark)
add_subdirectory(tutorial)