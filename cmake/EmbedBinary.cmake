if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED SYMBOL)
  message(FATAL_ERROR "EmbedBinary.cmake requires INPUT, OUTPUT, and SYMBOL")
endif()

file(READ "${INPUT}" binary_hex HEX)
string(REGEX MATCHALL ".." binary_bytes "${binary_hex}")
list(LENGTH binary_bytes byte_count)

get_filename_component(output_dir "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")
file(WRITE "${OUTPUT}"
  "// Generated from the pinned validator identity root; do not edit.\n"
  "#pragma once\n\n"
  "#include <array>\n"
  "#include <cstdint>\n\n"
  "namespace ripple::validator_identity {\n"
  "inline constexpr std::array<std::uint8_t, ${byte_count}> ${SYMBOL} = {\n")

foreach(byte IN LISTS binary_bytes)
  file(APPEND "${OUTPUT}" "0x${byte},")
endforeach()

file(APPEND "${OUTPUT}" "\n};\n}  // namespace ripple::validator_identity\n")
