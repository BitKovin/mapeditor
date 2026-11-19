# Usage: cmake -DSPV=in.spv -DOUT=out.spv.h -DNAME=lightmap_direct [-DHASH=<source hash>] [-DNS=core::shaders] -P spv_to_header.cmake
# NS is the C++ namespace of the array (default core::shaders; the native editor passes native::shaders).
# Turns a SPIR-V binary into a C++ header with a uint32_t array (little endian words). Pure CMake: no Python needed.
file(READ "${SPV}" hex HEX)
string(LENGTH "${hex}" len)
math(EXPR rem "${len} % 8")
if(NOT rem EQUAL 0)
  message(FATAL_ERROR "${SPV}: size is not a multiple of 4 bytes")
endif()
string(REGEX REPLACE "([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])" "0x\\4\\3\\2\\1," words "${hex}")
string(REGEX REPLACE "((0x[0-9a-f]+,)(0x[0-9a-f]+,)(0x[0-9a-f]+,)(0x[0-9a-f]+,)(0x[0-9a-f]+,)(0x[0-9a-f]+,)(0x[0-9a-f]+,)(0x[0-9a-f]+,))" "    \\1\n" words "${words}")
if(NOT NS)
  set(NS "core::shaders")
endif()
set(hashLine "")
if(HASH)
  set(hashLine "// source-sha256: ${HASH}\n")
endif()
file(WRITE "${OUT}" "// Generated from shaders/ (see shaders/manifest.txt). Do not edit.\n${hashLine}#pragma once\n#include <cstdint>\nnamespace ${NS} {\ninline const uint32_t k_${NAME}_spv[] = {\n${words}\n};\n}  // namespace ${NS}\n")
