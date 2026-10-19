#include "fsk_estimate.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sigcore {

FskEstimate estimate_fsk_params(const std::vector<std::complex<float>>& all) {
    if (all.size() < 256) throw std::runtime_error("estimate_fsk_params: capture too short");
    // The symbol-length search below costs ~2000x the samples it looks at, so look at a prefix:
    // tens of thousands of samples are plenty to see the tones and the symbol length, and a
    // multi-million-sample capture would otherwise hang the page.
    constexpr size_t kMaxSamples = 30000;
    const std::vector<std::complex<float>> x(all.begin(), all.begin() + static_cast<long>(std::min(all.size(), kMaxSamples)));
    const size_t n = x.size() - 1;
    std::vector<float> f(n);
    for (size_t i = 0; i < n; ++i) f[i] = static_cast<float>(std::arg(x[i + 1] * std::conj(x[i])) / (2.0 * M_PI));

    // Tone cluster centres from a smoothed histogram over [-0.5, 0.5).
    constexpr int kBins = 400;
    std::vector<double> hist(kBins, 0.0);
    for (float v : f) {
        int b = static_cast<int>((v + 0.5f) * kBins);
        hist[std::clamp(b, 0, kBins - 1)] += 1.0;
    }
    std::vector<double> sm(kBins, 0.0);
    for (int b = 0; b < kBins; ++b)
        for (int d = -2; d <= 2; ++d) sm[b] += hist[std::clamp(b + d, 0, kBins - 1)];
    const double peak = *std::max_element(sm.begin(), sm.end());
    std::vector<double> centres;
    for (int b = 1; b + 1 < kBins; ++b) {
        if (sm[b] >= 0.08 * peak && sm[b] >= sm[b - 1] && sm[b] > sm[b + 1]) {
            // merge peaks closer than 3 bins (plateaus)
            const double c = (b + 0.5) / kBins - 0.5;
            if (centres.empty() || c - centres.back() > 3.0 / kBins) centres.push_back(c);
        }
    }
    FskEstimate est;
    if (centres.size() < 2) return est;  // not FSK-like: confidence stays 0

    unsigned int m = 2;
    while (m < centres.size() && m < 16) m *= 2;
    if (centres.size() > m) m = std::min<size_t>(16, m * 2);
    est.bits_per_symbol = static_cast<unsigned int>(std::lround(std::log2(static_cast<double>(m))));
    // liquid's tones: -bw .. +bw, evenly spaced; use the outermost centres, mirrored.
    const double bw = 0.5 * (centres.back() - centres.front());
    est.bandwidth = static_cast<float>(bw);
    const double spacing = 2.0 * bw / static_cast<double>(m - 1);

    auto nearest = [&](float v) {
        const double k = std::round((v + bw) / spacing);
        return static_cast<int>(std::clamp<double>(k, 0.0, m - 1));
    };
    size_t near_tone = 0;
    for (float v : f) {
        const double tone = -bw + nearest(v) * spacing;
        if (std::fabs(v - tone) < 0.3 * spacing) ++near_tone;
    }
    est.confidence = static_cast<double>(near_tone) / static_cast<double>(n);

    // Symbol length: every DIVISOR of the true length gives pure fixed-grid windows too
    // (a constant tone stays constant in a shorter window), while multiples straddle symbol
    // changes and are impure -- so take the LARGEST candidate that is still (nearly) pure.
    const unsigned int lo = std::max<unsigned int>(m, 2), hi = 64;
    std::vector<double> score(hi + 1, 1e30);
    double best = 1e30;
    for (unsigned int s = lo; s <= hi; ++s) {
        double bestoff = 1e30;
        for (unsigned int off = 0; off < s; ++off) {
            double impure = 0.0;
            size_t windows = 0;
            for (size_t i = off; i + s <= n; i += s, ++windows) {
                std::vector<int> cnt(m, 0);
                for (unsigned int k = 0; k < s; ++k) ++cnt[nearest(f[i + k])];
                impure += 1.0 - static_cast<double>(*std::max_element(cnt.begin(), cnt.end())) / s;
            }
            if (windows) bestoff = std::min(bestoff, impure / static_cast<double>(windows));
        }
        score[s] = bestoff;
        best = std::min(best, bestoff);
    }
    for (unsigned int s = hi; s >= lo; --s) {
        if (score[s] <= best + 0.02) {
            est.samples_per_symbol = s;
            break;
        }
    }
    if (est.samples_per_symbol == 0) est.confidence = 0.0;
    return est;
}

}  // namespace sigcore
