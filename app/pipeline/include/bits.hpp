#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sigapp {

// A bitstream, one bit per element (0 or 1): the pipeline's working
// representation between demodulation and correlation.
using Bits = std::vector<uint8_t>;

}  // namespace sigapp
