#pragma once

#include <cstdint>
#include <vector>

namespace sigcore {

// Rate-1/2, constraint-length-7 convolutional code (the standard NASA/CCSDS
// "V27" code, generator polynomials 171/133 octal), hard-decision Viterbi
// decoded. Operates on unpacked bits (one bit per element, 0 or 1).
//
// Encoding appends K-1=6 zero tail bits to flush the encoder to the zero
// state, so encoded length = 2 * (msg_bits.size() + 6).
std::vector<uint8_t> viterbi_encode_bits(const std::vector<uint8_t>& msg_bits);

// `msg_len_bits` is the original (pre-tail) message length in bits.
std::vector<uint8_t> viterbi_decode_bits(const std::vector<uint8_t>& coded_bits,
                                          size_t msg_len_bits);

}  // namespace sigcore
