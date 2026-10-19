#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sigcore {

// Gap-list Phase 5. Punctured versions of viterbi.hpp's rate-1/2, K=7 (171/133) code.
//  rate 2/3: keep A1 B1 A2        of every (A1 B1 A2 B2)
//  rate 3/4: keep A1 B1 B2 A3     of every (A1 B1 A2 B2 A3 B3)
// -- the usual puncture matrices {11,10} and {110,101} for this mother code. Decoding
// re-inserts the dropped bits as erasures (zero LLR) and runs the soft decoder.
// `num`/`den` select the rate: (2,3) or (3,4); anything else throws std::invalid_argument.
std::vector<uint8_t> viterbi_punctured_encode(const std::vector<uint8_t>& msg_bits, unsigned num, unsigned den);
std::vector<uint8_t> viterbi_punctured_decode(const std::vector<uint8_t>& coded_bits, size_t msg_len_bits,
                                              unsigned num, unsigned den);
// Encoded length for a message of `msg_len_bits` bits.
size_t viterbi_punctured_length(size_t msg_len_bits, unsigned num, unsigned den);

// Reed-Solomon RS(255,223) over GF(256) (field polynomial 0x11D, generator roots
// alpha^1..alpha^32), systematic, corrects up to 16 byte errors per block. Messages are
// split into 223-byte blocks (the last zero-padded); decode truncates to `msg_len` bytes
// and throws std::runtime_error for a block with more errors than it can correct.
// NOTE: this is the common "conventional basis" RS(255,223), NOT the CCSDS dual-basis
// variant -- no standards test vector was available to check against, so interoperability
// with a specific standard's streams is unverified.
std::vector<uint8_t> rs255_encode_bytes(const std::vector<uint8_t>& msg);
std::vector<uint8_t> rs255_decode_bytes(const std::vector<uint8_t>& encoded, size_t msg_len);

}  // namespace sigcore
