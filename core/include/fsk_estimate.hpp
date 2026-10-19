#pragma once

#include <complex>
#include <vector>

namespace sigcore {

// Gap-list Phase 2.10: blind M-FSK parameters from the raw capture alone, in exactly the
// terms fsk_modulate/fsk_demodulate take: bits per symbol, samples per symbol and
// "bandwidth" (liquid's fskmod puts its outermost tones at +-bandwidth cycles/sample).
struct FskEstimate {
    unsigned int bits_per_symbol = 1;
    unsigned int samples_per_symbol = 0;
    float bandwidth = 0.0f;
    double confidence = 0.0;  // 0..1: share of samples sitting near an estimated tone
};

// Looks at the first 30,000 samples only (the search is ~2000x as costly as its input).
// Throws std::runtime_error for a capture too short (< 256 samples). Assumes continuous-
// phase M-FSK with M a power of two in [2, 16] and evenly spaced tones; anything else
// yields low confidence, not an exception -- callers should check `confidence`.
FskEstimate estimate_fsk_params(const std::vector<std::complex<float>>& samples);

}  // namespace sigcore
