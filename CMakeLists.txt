cmake_minimum_required(VERSION 3.5.1)

project(rgd C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -g -std=c++17 -mcx16 -march=native -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free")
# set(CMAKE_LINKER_FLAGS "${CMAKE_LINKER_FLAGS} -fsanitize=address")
set(CMAKE_TEST_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/test)

find_package(LLVM 12 REQUIRED CONFIG)

if (LLVM_FOUND)
  message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
  message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
else()
  message(FATAL_ERROR "You haven't install LLVM !")
endif()


include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# Proto file
get_filename_component(rgd_proto "rgd.proto" ABSOLUTE)
get_filename_component(rgd_proto_path "${rgd_proto}" PATH)

add_custom_target(proto ALL DEPENDS ${rgd_proto_srcs} ${rgd_proto_hdrs})
# Generated sources
set(rgd_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/rgd.pb.cc")
set(rgd_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/rgd.pb.h")
add_custom_command(
  OUTPUT "${rgd_proto_srcs}" "${rgd_proto_hdrs}"
  COMMAND protoc
  ARGS --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
    -I "${rgd_proto_path}"
    "${rgd_proto}"
  DEPENDS "${rgd_proto}")

# Include generated *.pb.h files
include_directories("${CMAKE_CURRENT_BINARY_DIR}")

add_library(gd
  gd.cc
  input.cc
  grad.cc
)

add_executable(rgd
  rgd.cc
  jit.cc
  util.cc
  xxhash.cc
  ${rgd_proto_srcs})

target_link_libraries(rgd
  protobuf
  tcmalloc
  profiler
  gd
  LLVM
  pthread
  boost_system
  boost_filesystem)
