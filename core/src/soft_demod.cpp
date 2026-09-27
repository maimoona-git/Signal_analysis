#include "soft_demod.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace sigcore {

namespace {

// Never let a noiseless (or near-noiseless) capture produce an infinite LLR: the FEC
// decoders below treat LLR magnitude as confidence, and a genuinely infinite value is
// no more useful to them than a very large, finite one.
constexpr float kMinSigma2 = 1e-6f;

// Per-bit max-log LLRs from a per-candidate-symbol squared distance: for bit position
// b (0 = MSB, matching bits_to_symbols/symbols_to_bits), LLR_b = (closest distance
// among symbols with bit_b=1 - closest among symbols with bit_b=0) / (2*sigma2). One
// function serves both the linear and FSK paths; only how `distance` is computed
// differs between them.
LLRBuffer llr_from_distances(const std::vector<float>& distance, unsigned int bps, float sigma2) {
    LLRBuffer out(bps);
    const float denom = 2.0f * std::max(sigma2, kMinSigma2);
    for (unsigned int b = 0; b < bps; ++b) {
        const unsigned int shift = bps - 1 - b;
        float min0 = std::numeric_limits<float>::infinity();
        float min1 = std::numeric_limits<float>::infinity();
        for (unsigned int m = 0; m < distance.size(); ++m) {
            const bool bit_is_one = ((m >> shift) & 1u) != 0;
            float& slot = bit_is_one ? min1 : min0;
            slot = std::min(slot, distance[m]);
        }
        out[b] = (min1 - min0) / denom;
    }
    return out;
}

}  // namespace

std::vector<uint8_t> llr_hard_decision(const LLRBuffer& llrs) {
    std::vector<uint8_t> bits(llrs.size());
    for (size_t i = 0; i < llrs.size(); ++i) bits[i] = llrs[i] < 0.0f ? 1 : 0;
    return bits;
}

float estimate_noise_variance_linear(const std::vector<std::complex<float>>& samples, ModulationScheme scheme) {
    if (samples.empty()) {
        throw std::runtime_error("estimate_noise_variance_linear: samples is empty");
    }
    const auto hard_bits = linear_demodulate(samples, scheme);
    const auto ideal = linear_modulate(hard_bits, scheme);  // may be a few bits shorter (padding)

    double sum_sq_error = 0.0;
    const size_t n = std::min(samples.size(), ideal.size());
    for (size_t i = 0; i < n; ++i) sum_sq_error += std::norm(samples[i] - ideal[i]);
    return static_cast<float>(std::max(sum_sq_error / (2.0 * n), static_cast<double>(kMinSigma2)));
}

LLRBuffer soft_demodulate_linear(const std::vector<std::complex<float>>& samples, ModulationScheme scheme,
                                 float sigma2) {
    if (samples.empty()) {
        throw std::runtime_error("soft_demodulate_linear: samples is empty");
    }
    const auto points = linear_constellation_points(scheme);
    const unsigned int bps = linear_bits_per_symbol(scheme);

    LLRBuffer out;
    out.reserve(samples.size() * bps);
    std::vector<float> distance(points.size());
    for (const auto& y : samples) {
        for (size_t m = 0; m < points.size(); ++m) distance[m] = std::norm(y - points[m]);
        const auto llrs = llr_from_distances(distance, bps, sigma2);
        out.insert(out.end(), llrs.begin(), llrs.end());
    }
    return out;
}

}  // namespace sigcore
