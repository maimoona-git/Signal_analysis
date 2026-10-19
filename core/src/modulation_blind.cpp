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


namespace {

// log I0(x), stable for large x (Abramowitz & Stegun 9.8.1 / 9.8.2).
double log_i0(double x) {
    x = std::fabs(x);
    if (x < 3.75) {
        const double t = (x / 3.75) * (x / 3.75);
        return std::log(1.0 + t * (3.5156229 + t * (3.0899424 + t * (1.2067492 + t * (0.2659732 + t * (0.0360768 + t * 0.0045813))))));
    }
    const double t = 3.75 / x;
    const double p = 0.39894228 + t * (0.01328592 + t * (0.00225319 + t * (-0.00157565 + t * (0.00916281 + t * (-0.02057706 +
                     t * (0.02635537 + t * (-0.01647633 + t * 0.00392377)))))));
    return x + std::log(p) - 0.5 * std::log(x);
}

// Unit-power ring radii and weights of the square QAM with `side` levels per axis.
void qam_rings(int side, std::vector<double>& radius, std::vector<double>& weight) {
    std::vector<double> r2;
    for (int i = 0; i < side; ++i)
        for (int q = 0; q < side; ++q) {
            const double a = 2 * i - side + 1, b = 2 * q - side + 1;
            r2.push_back(a * a + b * b);
        }
    double mean = 0.0;
    for (double v : r2) mean += v;
    mean /= static_cast<double>(r2.size());
    std::sort(r2.begin(), r2.end());
    for (size_t k = 0; k < r2.size();) {
        size_t j = k;
        while (j < r2.size() && std::fabs(r2[j] - r2[k]) < 1e-9) ++j;
        radius.push_back(std::sqrt(r2[k] / mean));
        weight.push_back(static_cast<double>(j - k) / static_cast<double>(r2.size()));
        k = j;
    }
}

// Best log-likelihood, over the signal share S of the (unit) power, of the symbol magnitudes `r`
// under rings of the given radii: Rician noise with per-axis variance (1 - S) / 2. Independent of
// the carrier phase, which is why it can separate QAM (several rings) from a noisy PSK (one ring)
// where every cumulant of the two is the same function of S.
double ring_loglik(const std::vector<double>& r, const std::vector<double>& radius, const std::vector<double>& weight) {
    double best = -1e300;
    for (double S = 0.30; S <= 1.0001; S += 0.02) {
        const double var = std::max((1.0 - S) / 2.0, 1e-4);
        double ll = 0.0;
        for (double x : r) {
            double p = 0.0;
            for (size_t k = 0; k < radius.size(); ++k) {
                const double rho = std::sqrt(S) * radius[k];
                const double lg = std::log(x / var + 1e-300) - (x * x + rho * rho) / (2.0 * var) + log_i0(x * rho / var);
                p += weight[k] * std::exp(lg);
            }
            ll += std::log(p + 1e-300);
        }
        best = std::max(best, ll);
    }
    return best;
}

}  // namespace

double spectral_peakiness(const std::vector<std::complex<float>>& samples) {
    constexpr size_t kSeg = 64;
    const size_t n = std::min<size_t>(samples.size(), 65536);
    if (n < 2 * kSeg) return 0.0;
    std::vector<double> psd(kSeg, 0.0);
    std::vector<std::complex<float>> seg(kSeg);
    for (size_t s = 0; s + kSeg <= n; s += kSeg / 2) {
        for (size_t i = 0; i < kSeg; ++i) {
            const double w = 0.5 - 0.5 * std::cos(2.0 * M_PI * static_cast<double>(i) / (kSeg - 1));
            seg[i] = samples[s + i] * static_cast<float>(w);
        }
        const auto spectrum = fft_forward(seg);
        for (size_t i = 0; i < kSeg; ++i) psd[i] += std::norm(spectrum[i]);
    }
    double peak = 0.0, mean = 0.0;
    for (double d : psd) {
        peak = std::max(peak, d);
        mean += d;
    }
    mean /= static_cast<double>(kSeg);
    return mean > 0.0 ? peak / mean : 0.0;
}

ConstellationCheck constellation_check(const std::vector<std::complex<float>>& symbols) {
    ConstellationCheck out;
    if (symbols.size() < 100) return out;
    using cd = std::complex<double>;
    const size_t n = std::min<size_t>(symbols.size(), 65536);
    double c21 = 0.0;
    for (size_t i = 0; i < n; ++i) c21 += std::norm(cd(symbols[i]));
    c21 /= static_cast<double>(n);
    if (!(c21 > 0.0)) return out;
    const double inv = 1.0 / std::sqrt(c21);
    cd m2 = 0.0, m4c = 0.0;
    double m4 = 0.0;
    std::vector<double> mag;
    mag.reserve(std::min<size_t>(n, 4096));
    const size_t mag_stride = std::max<size_t>(1, n / 4096);
    for (size_t i = 0; i < n; ++i) {
        const cd v = cd(symbols[i]) * inv;
        const cd v2 = v * v;
        m2 += v2;
        m4c += v2 * v2;
        m4 += std::norm(v) * std::norm(v);
        if (i % mag_stride == 0) mag.push_back(std::abs(v));
    }
    m2 /= static_cast<double>(n);
    m4c /= static_cast<double>(n);
    m4 /= static_cast<double>(n);
    out.c40 = std::abs(m4c - 3.0 * m2 * m2);
    out.c42 = m4 - std::norm(m2) - 2.0;

    // Step 1, phase-only schemes: the cumulant fit (ka = 1, one ring).
    struct Hyp { const char* name; double c40, c42; };
    static const Hyp psk[] = {{"bpsk", 2.0, -2.0}, {"qpsk", 1.0, -1.0}, {"psk8", 0.0, -1.0}};
    const double s2_psk = std::min(1.0, std::max(0.0, 2.0 - m4));
    std::vector<std::pair<double, std::string>> fit;
    for (const Hyp& h : psk)
        fit.push_back({std::pow(out.c40 - h.c40 * s2_psk, 2) + std::pow(out.c42 - h.c42 * s2_psk, 2), h.name});
    std::sort(fit.begin(), fit.end());

    // Step 2, one ring (any PSK) against the QAM ring structures, by likelihood of the magnitudes.
    std::vector<double> r1 = {1.0}, w1 = {1.0}, r16, w16, r64, w64;
    qam_rings(4, r16, w16);
    qam_rings(8, r64, w64);
    const double ll_psk = ring_loglik(mag, r1, w1);
    const double ll16 = ring_loglik(mag, r16, w16);
    const double ll64 = ring_loglik(mag, r64, w64);

    std::vector<std::pair<double, std::string>> order = {{ll_psk, fit[0].second}, {ll16, "qam16"}, {ll64, "qam64"}};
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    out.best = order[0].second;
    out.margin = (order[0].first - order[1].first) / static_cast<double>(mag.size());
    for (const auto& o : order) out.ranking.push_back(o.second);
    for (const auto& f : fit)
        if (std::find(out.ranking.begin(), out.ranking.end(), f.second) == out.ranking.end()) out.ranking.push_back(f.second);
    return out;
}

}  // namespace sigcore
