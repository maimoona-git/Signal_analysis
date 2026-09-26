#pragma once

// Port of app/fec.py: FEC encode/decode by scheme *name*, always on bits.
// Byte-oriented schemes (rs, concatenated) pack bits MSB-first into bytes,
// zero-padding the last byte, exactly like the original. `msg_len` is in bits
// for viterbi/ldpc and in bytes for rs/concatenated (each scheme's native unit).

#include <cstddef>
#include <string>

#include "bits.hpp"

namespace sigapp {

// "viterbi", "ldpc", "rs", "concatenated" (case-insensitive). Throws
// std::invalid_argument for an unknown scheme; sigcore throws
// std::runtime_error for malformed input (e.g. a misaligned length).
Bits fec_encode(const Bits& bits, const std::string& scheme);
Bits fec_decode(const Bits& bits, const std::string& scheme, size_t msg_len);

}  // namespace sigapp
