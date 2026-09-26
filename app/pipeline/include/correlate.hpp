#pragma once

// Port of app/correlate.py: locating repeating headers / frame periods in a
// decoded bitstream. Inputs are 0/1 bits; anything nonzero counts as 1.
//
// Errors mirror the original: where it raises ValueError this throws
// std::invalid_argument.

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "bits.hpp"

namespace sigapp {

struct FrameBoundaries {
    Bits header_pattern;
    std::optional<int> frame_period;
    std::vector<size_t> header_positions;
    std::vector<Bits> payloads;
};

// Normalised autocorrelation of the +/-1-mapped bits for lags 1..n-1 (lag 0,
// always 1, is dropped). Needs n >= 2. For +/-1 data every correlation sum is an
// exact integer, so this is computed exactly (XOR + popcount on packed words,
// ~64x faster than the O(n^2) multiply-add) and matches numpy bit for bit.
std::vector<double> autocorrelation(const Bits& bits);

// Strongest autocorrelation peak for lags in [min_period, max_period]
// (max_period defaults to n/2; a value of 0 also means "default", like the
// original's `max_period or n // 2`). Returns (period, strength); the earliest
// lag wins ties. Throws if the range is empty or the signal too short.
std::pair<int, double> detect_frame_period(const Bits& bits, int min_period = 4,
                                           std::optional<int> max_period = std::nullopt);

// Start positions where `pattern` occurs in `bits` with at most
// `max_bit_errors` mismatches (Hamming distance). Empty for an empty or longer pattern.
std::vector<size_t> find_pattern_occurrences(const Bits& bits, const Bits& pattern, int max_bit_errors = 0);

// Packs bits MSB-first into bytes (a ragged tail is dropped) and returns them as
// ASCII text if every byte is printable (or \n \r \t), otherwise as a hex string.
std::string bits_to_text(const Bits& bits);

// If `header_pattern` is absent it is assumed to be the first `header_len` bits.
// The frame period is the most common gap between occurrences (smallest gap on
// ties), falling back to autocorrelation when fewer than two occurrences exist.
FrameBoundaries find_header_payload_boundaries(const Bits& bits,
                                               const std::optional<Bits>& header_pattern = std::nullopt,
                                               size_t header_len = 16, int max_bit_errors = 1,
                                               int min_period = 4);

}  // namespace sigapp
