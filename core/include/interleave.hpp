#pragma once

#include <cstdint>
#include <vector>

namespace sigcore {

// Block interleaver: writes bits into a rows x cols matrix row-wise, reads
// out column-wise. bits.size() must equal rows * cols.
std::vector<uint8_t> block_interleave(const std::vector<uint8_t>& bits, unsigned int rows,
                                       unsigned int cols);
std::vector<uint8_t> block_deinterleave(const std::vector<uint8_t>& bits, unsigned int rows,
                                         unsigned int cols);

// Diagonal interleaver: writes bits into a rows x cols matrix row-wise, then
// cyclically shifts row r right by r positions (mod cols) before reading out
// column-wise. bits.size() must equal rows * cols.
std::vector<uint8_t> diagonal_interleave(const std::vector<uint8_t>& bits, unsigned int rows,
                                          unsigned int cols);
std::vector<uint8_t> diagonal_deinterleave(const std::vector<uint8_t>& bits, unsigned int rows,
                                            unsigned int cols);

// Pseudo-random interleaver: permutes bits according to a fixed pseudo-random
// permutation determined by `seed` (deterministic and reproducible).
std::vector<uint8_t> pseudo_random_interleave(const std::vector<uint8_t>& bits,
                                               unsigned int seed);
std::vector<uint8_t> pseudo_random_deinterleave(const std::vector<uint8_t>& bits,
                                                 unsigned int seed);

// Convolutional (Forney/cross) interleaver: bit i is routed cyclically to
// branch (i % branches); branch b holds a FIFO of b * delay_increment
// branch-local slots (i.e. b * delay_increment * branches bits of global
// latency, since a branch is only visited once every `branches` bits).
// Combined with a matching deinterleaver (branch b delayed by
// (branches - 1 - b) * delay_increment instead), every bit sees the same
// total end-to-end latency of branches * (branches - 1) * delay_increment
// bits: the deinterleaver's first that many output bits are the FIFOs'
// zero-initialized flush content, not real data, and the true recovered
// stream begins after that offset.
std::vector<uint8_t> convolutional_interleave(const std::vector<uint8_t>& bits,
                                               unsigned int branches,
                                               unsigned int delay_increment);
std::vector<uint8_t> convolutional_deinterleave(const std::vector<uint8_t>& bits,
                                                 unsigned int branches,
                                                 unsigned int delay_increment);

}  // namespace sigcore
