#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sigcore {

// NOTE: liquid-dsp's bundled `fec`/`packetizer` objects for convolutional
// (Viterbi) and Reed-Solomon codes require the external `libfec` library
// (Phil Karn's), which is not packaged for Homebrew and isn't installed on
// this system (confirmed: `fec_create(LIQUID_FEC_CONV_V27, ...)` and
// `fec_create(LIQUID_FEC_RS_M8, ...)` both return NULL here). Rather than
// depend on a fragile external native build, Viterbi and Reed-Solomon are
// implemented directly (see viterbi.hpp / reed_solomon.hpp); LDPC has no
// liquid-dsp equivalent at all and is likewise hand-rolled.

// Rate-1/2 K=7 convolutional code (Viterbi-decoded). Operates on unpacked
// bits (one bit per element, 0 or 1). See viterbi.hpp for details.
std::vector<uint8_t> viterbi_encode(const std::vector<uint8_t>& msg_bits);
std::vector<uint8_t> viterbi_decode(const std::vector<uint8_t>& coded_bits, size_t msg_len_bits);

// Reed-Solomon (15,13) over GF(256), t=1. Operates on byte messages.
// See reed_solomon.hpp for details.
std::vector<uint8_t> rs_encode(const std::vector<uint8_t>& msg);
std::vector<uint8_t> rs_decode(const std::vector<uint8_t>& encoded, size_t msg_len);

// Concatenated code: outer Reed-Solomon (byte-level) + inner Viterbi
// (bit-level). Output/input of both functions is bits (one bit per
// element), since that's the natural representation once the inner
// convolutional code is applied.
std::vector<uint8_t> concatenated_encode(const std::vector<uint8_t>& msg);
std::vector<uint8_t> concatenated_decode(const std::vector<uint8_t>& encoded_bits, size_t msg_len);

// Small hand-rolled regular LDPC block code (rate 1/2, k=8 message bits,
// n=16 codeword bits per block), systematically encoded via a staircase
// parity-check matrix and decoded with Gallager's bit-flipping algorithm
// (Algorithm A). Operates on unpacked bits (one bit per element, 0 or 1).
// `msg_bits` is padded with zeros to a multiple of 8 before encoding;
// `msg_len_bits` truncates the decoded output back to the original length.
std::vector<uint8_t> ldpc_encode(const std::vector<uint8_t>& msg_bits);
std::vector<uint8_t> ldpc_decode(const std::vector<uint8_t>& encoded_bits, size_t msg_len_bits);

// Feature 3 (SIH26147 extra feature, soft-decision decoding): the same code, decoded
// from channel LLRs with normalised min-sum belief propagation instead of Gallager
// bit-flipping. `encoded_llrs.size()` must be a multiple of n=16, exactly like
// ldpc_decode's bit-count requirement.
std::vector<uint8_t> ldpc_decode_soft(const std::vector<float>& encoded_llrs, size_t msg_len_bits);

}  // namespace sigcore
