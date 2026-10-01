// The GAIKA handle. See include/imperivm/core/sim/gaika.hpp for the evidence
// that a GAIKA is an integer index and for what this file deliberately omits.

#include "imperivm/core/sim/gaika.hpp"

namespace imperivm::core::sim {

script::Value gaika_value(GaikaId id) noexcept {
  return script::Value::integer(id > 0 ? id : kNoGaika);
}

GaikaId gaika_of(const script::Value& value) noexcept {
  if (!value.is_integer()) return kNoGaika;
  const std::int32_t id = value.as_integer();
  return id > 0 ? id : kNoGaika;
}

}  // namespace imperivm::core::sim
