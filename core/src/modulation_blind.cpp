#include "modulation_blind.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "dsp.hpp"
#include "fsk_estimate.hpp"

namespace sigcore {

namespace {

constexpr size_t kMaxSamples = 1u << 17;

// Share of the k-th power's spectral energy in the strongest bin and its two neighbours (the
// neighbours catch a line that falls between bins).
double power_line(const std::vector<std::complex<float>>& x, int k) {
    const size_t n = std::min(x.size(), kMaxSamples);
    size_t m = 1;
    while (m < n) m <<= 1;
    std::vector<std::complex<float>> p(m, std::complex<float>(0.0f, 0.0f));
    for (size_t i = 0; i < n; ++i) {
        std::complex<float> q = x[i];
        for (int j = 1; j < k; ++j) q *= x[i];
        p[i] = q;
    }
    const auto spectrum = fft_forward(p);
    std::vector<double> e(m);
    double total = 0.0;
    for (size_t i = 0; i < m; ++i) {
        e[i] = std::norm(spectrum[i]);
        total += e[i];
    }
    if (total <= 0.0) return 0.0;
    const size_t peak = static_cast<size_t>(std::max_element(e.begin(), e.end()) - e.begin());
    return (e[peak] + e[(peak + 1) % m] + e[(peak + m - 1) % m]) / total;
}

double envelope_cv(const std::vector<std::complex<float>>& x) {
    const size_t n = std::min(x.size(), kMaxSamples);
    std::vector<float> mag(n);
    for (size_t i = 0; i < n; ++i) mag[i] = std::abs(x[i]);
    std::vector<float> sorted = mag;
    std::sort(sorted.begin(), sorted.end());
    const float threshold = sorted[n * 4 / 10];  // keep the strongest 60%: idle noise around a burst must not count
    double sum = 0.0, sq = 0.0;
    size_t count = 0;
    for (float v : mag) {
        if (v >= threshold) {
            sum += v;
            sq += static_cast<double>(v) * v;
            ++count;
        }
    }
    if (count == 0 || sum <= 0.0) return 0.0;
    const double mean = sum / static_cast<double>(count);
    return std::sqrt(std::max(0.0, sq / static_cast<double>(count) - mean * mean)) / mean;
}

}  // namespace

BlindLinearCheck blind_linear_check(const std::vector<std::complex<float>>& samples) {
    BlindLinearCheck r;
    if (samples.size() < 1024) return r;
    try {
        r.envelope_cv = envelope_cv(samples);
        try {
            r.fsk_confidence = estimate_fsk_params(samples).confidence;
        } catch (const std::exception&) {
            r.fsk_confidence = 0.0;  // could not run: no evidence FOR fsk
        }
        r.line2 = power_line(samples, 2);
        r.line4 = power_line(samples, 4);
        r.line8 = power_line(samples, 8);
    } catch (const std::exception&) {
        return BlindLinearCheck{};
    }
    // all three tests must agree it is not FSK before the lines are read
    if (r.fsk_confidence >= 0.8 || r.envelope_cv < 0.3) return r;
    if (r.line2 >= 0.15) r.family = "bpsk";
    else if (r.line2 < 0.05 && r.line4 >= 0.08) r.family = "qpsk";
    else if (r.line2 < 0.05 && r.line4 < 0.01 && r.line8 >= 0.05) r.family = "psk8";
    return r;
}

}  // namespace sigcore
