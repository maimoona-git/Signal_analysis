#pragma once

#include <cstdint>
#include <vector>

namespace sigcore {

// Single-error-correcting Reed-Solomon block code over GF(256), n=15
// symbols (bytes), k=13 message symbols, t=1 (2 parity symbols per block).
// Systematic: codeword = [message (13 bytes) | parity (2 bytes)].
// Arbitrary-length messages are split into k-byte blocks (zero-padded in
// the last block).
std::vector<uint8_t> rs_encode_bytes(const std::vector<uint8_t>& msg);

// Decodes n-byte blocks back to k-byte message blocks, correcting up to
// one byte error per block, then truncates to `msg_len` bytes.
std::vector<uint8_t> rs_decode_bytes(const std::vector<uint8_t>& encoded, size_t msg_len);

}  // namespace sigcore
