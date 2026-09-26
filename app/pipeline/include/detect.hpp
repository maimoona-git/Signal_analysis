#pragma once

// Port of app/detect.py: blind interleaver/FEC parameter detection.
//
// A bounded candidate search, not general blind recognition: every candidate
// interleaver (block/diagonal over divisor pairs of the length, convolutional,
// a bounded sweep of pseudo-random seeds) x every FEC scheme (or none) is
// tried, FEC hypotheses must round-trip (re-encode within 5% BER), and
// candidates are ranked by how often the stream's own first bits recur as a
// header. Ties break in favour of the more specific FEC. The search order, the
// scoring and the tie-breaks are the original's, decision for decision:
// tests/data/pipeline/detect.rec holds 90 cases recorded from the original.

#include <cstddef>
#include <optional>
#include <string>

#include "bits.hpp"
#include "interleave_dispatch.hpp"

namespace sigapp {

struct DetectionResult {
    std::optional<InterleaverSpec> interleaver;  // nullopt = "no interleaver detected"
    std::optional<std::string> fec;
    std::optional<size_t> fec_msg_len;
    int score = 0;
    std::optional<Bits> deinterleaved_bits;
    std::optional<Bits> decoded_bits;
    Bits working_bits;
};

// `header_len` defaults to min(16, max(4, n / 8)) (a value of 0 also means
// default). Below `score_floor` nothing is reported: interleaver and fec are
// nullopt and working_bits is the input. Throws std::invalid_argument on empty input.
DetectionResult detect_params(const Bits& bits, std::optional<size_t> header_len = std::nullopt,
                              int score_floor = 3, unsigned int pseudo_random_seed_range = 32);

}  // namespace sigapp
