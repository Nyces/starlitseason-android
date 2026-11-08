# 把二进制文件转成 C++ 无符号字符数组（用于把 SPIR-V 嵌进可执行文件）。
#
# 用法：
#   cmake -DINPUT=scene.vert.spv -DOUTPUT=scene_vert_spirv.h -DNAME=kSceneVertexSpirv \
#         -P embed_binary.cmake

if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED NAME)
  message(FATAL_ERROR "embed_binary.cmake 需要 INPUT / OUTPUT / NAME")
endif()

file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" hex_length)
math(EXPR byte_length "${hex_length} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")

file(WRITE "${OUTPUT}"
  "// 由 CMake 自动生成，请勿手改。\n"
  "#pragma once\n"
  "static const unsigned char ${NAME}[] = {${bytes}};\n"
  "static const unsigned int ${NAME}_size = ${byte_length}u;\n")
