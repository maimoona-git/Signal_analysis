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

// Feature 3 (SIH26147 extra feature, soft-decision decoding): the same code, decoded
// from LLRs instead of hard bits. The branch metric a hard decision would compute as a
// Hamming distance against a 0/1 received bit becomes, per coded bit, `expected == 0 ?
// -llr : llr` against the received LLR (smaller is a better match, exactly the same
// convention as the Hamming-distance metric, just continuous instead of {0, 1}): the
// trellis and traceback are otherwise identical to viterbi_decode_bits.
std::vector<uint8_t> viterbi_decode_soft(const std::vector<float>& coded_llrs, size_t msg_len_bits);

}  // namespace sigcore
