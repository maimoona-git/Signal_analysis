#pragma once

// Port of app/fec.py: FEC encode/decode by scheme *name*, always on bits.
// Byte-oriented schemes (rs, concatenated) pack bits MSB-first into bytes,
// zero-padding the last byte, exactly like the original. `msg_len` is in bits
// for viterbi/ldpc and in bytes for rs/concatenated (each scheme's native unit).

#include <cstddef>
#include <string>

#include "bits.hpp"
#include "soft_demod.hpp"  // sigcore::LLRBuffer

namespace sigapp {

// "viterbi", "ldpc", "rs", "concatenated" (case-insensitive). Throws
// std::invalid_argument for an unknown scheme; sigcore throws
// std::runtime_error for malformed input (e.g. a misaligned length).
Bits fec_encode(const Bits& bits, const std::string& scheme);
Bits fec_decode(const Bits& bits, const std::string& scheme, size_t msg_len);

// True for the schemes soft decoding is implemented for ("viterbi", "ldpc"); RS decodes
// on symbols with hard decisions (there is no soft Reed-Solomon here), and concatenated
// pairs RS with Viterbi, so it inherits that same limit.
bool fec_supports_soft_decode(const std::string& scheme);

// Feature 3 (SIH26147 extra feature, soft-decision decoding): decodes from channel LLRs
// instead of hard bits, for the schemes fec_supports_soft_decode allows. Throws
// std::invalid_argument for "rs"/"concatenated" or an unknown scheme.
Bits fec_decode_soft(const sigcore::LLRBuffer& llrs, const std::string& scheme, size_t msg_len);

}  // namespace sigapp
