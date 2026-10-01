#include "imperivm/core/script/value.hpp"

namespace imperivm::core::script {

std::string to_decimal(std::int32_t value) {
  // Hand-rolled rather than std::to_string: the core has no locale to depend
  // on and no I/O headers to include, and script-visible strings end up in
  // hashed world state, so the spelling has to be the same everywhere.
  if (value == 0) return "0";
  const bool negative = value < 0;
  // Negated in unsigned space so that INT32_MIN does not overflow on the way.
  std::uint32_t magnitude = negative ? 0u - static_cast<std::uint32_t>(value)
                                     : static_cast<std::uint32_t>(value);
  char buffer[12];
  int at = 12;
  while (magnitude != 0) {
    buffer[--at] = static_cast<char>('0' + (magnitude % 10));
    magnitude /= 10;
  }
  std::string out;
  if (negative) out.push_back('-');
  out.append(buffer + at, static_cast<std::size_t>(12 - at));
  return out;
}

}  // namespace imperivm::core::script
