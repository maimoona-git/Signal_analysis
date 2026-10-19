#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sigcore {

// CCSDS 131.0-B Reed-Solomon RS(255,223): GF(256) with field polynomial 0x187, generator
// roots (alpha^11)^(112..143), systematic, corrects up to 16 byte errors per block. Messages
// are split into 223-byte blocks (the last zero-padded). With `dual_basis` true, the bytes on
// the wire use Berlekamp's dual basis, as the standard transmits them; false is the plain
// conventional basis. Decode truncates to `msg_len` bytes and throws std::runtime_error for a
// block with more errors than it can correct.
// Only the chain "RS then K=7 rate-1/2 Viterbi" is implemented; the standard's interleaving
// depth, pseudo-randomiser and sync marker are not.
std::vector<uint8_t> rs_ccsds_encode_bytes(const std::vector<uint8_t>& msg, bool dual_basis = true);
std::vector<uint8_t> rs_ccsds_decode_bytes(const std::vector<uint8_t>& encoded, size_t msg_len,
                                           bool dual_basis = true);

// RS(255,223) followed by the CCSDS rate-1/2 K=7 convolutional code (viterbi.hpp).
std::vector<uint8_t> ccsds_concat_encode(const std::vector<uint8_t>& msg_bytes);
std::vector<uint8_t> ccsds_concat_decode(const std::vector<uint8_t>& coded_bits, size_t msg_len_bytes);

}  // namespace sigcore
