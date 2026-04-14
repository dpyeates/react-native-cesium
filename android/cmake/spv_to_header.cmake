# spv_to_header.cmake — Convert a .spv binary to a C header with an embedded uint32_t array.
# Input variables: SPV_FILE, SPV_HEADER, ARRAY_NAME

file(READ "${SPV_FILE}" SPV_DATA HEX)
string(LENGTH "${SPV_DATA}" SPV_HEX_LEN)

# Validate SPIR-V magic (0x07230203, little-endian on disk → "03022307")
string(SUBSTRING "${SPV_DATA}" 0 8 SPV_MAGIC)
string(TOLOWER "${SPV_MAGIC}" SPV_MAGIC_LC)
if(NOT SPV_MAGIC_LC STREQUAL "03022307")
  message(FATAL_ERROR "spv_to_header: '${SPV_FILE}' does not start with the SPIR-V magic number (got '${SPV_MAGIC}'). The file may be corrupt or not a valid SPIR-V binary.")
endif()
math(EXPR SPV_BYTE_COUNT "${SPV_HEX_LEN} / 2")
math(EXPR SPV_WORD_COUNT "${SPV_BYTE_COUNT} / 4")

set(HEADER_CONTENT "#pragma once\n#include <cstdint>\nstatic const uint32_t ${ARRAY_NAME}[] = {\n")

set(I 0)
while(I LESS SPV_HEX_LEN)
  # Read 8 hex chars (4 bytes = 1 uint32_t word, little-endian in SPIR-V)
  string(SUBSTRING "${SPV_DATA}" ${I} 8 WORD_HEX)
  # SPIR-V is stored little-endian; file(READ HEX) gives bytes in order,
  # so we need to reverse byte order for the uint32_t literal.
  string(SUBSTRING "${WORD_HEX}" 0 2 B0)
  string(SUBSTRING "${WORD_HEX}" 2 2 B1)
  string(SUBSTRING "${WORD_HEX}" 4 2 B2)
  string(SUBSTRING "${WORD_HEX}" 6 2 B3)
  set(WORD "0x${B3}${B2}${B1}${B0}")

  math(EXPR NEXT_I "${I} + 8")
  if(NEXT_I LESS SPV_HEX_LEN)
    string(APPEND HEADER_CONTENT "  ${WORD},\n")
  else()
    string(APPEND HEADER_CONTENT "  ${WORD}\n")
  endif()
  set(I ${NEXT_I})
endwhile()

string(APPEND HEADER_CONTENT "};\nstatic const uint32_t ${ARRAY_NAME}_size = sizeof(${ARRAY_NAME});\n")

file(WRITE "${SPV_HEADER}" "${HEADER_CONTENT}")
