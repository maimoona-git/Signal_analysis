// Phase 2: known synthetic signals must produce the correct FFT / spectral
// output. Runs on the native build (Homebrew liquid-dsp, fftw-backed FFT) and
// on the WASM build (liquid-dsp's internal FFT), so the tolerances below are
// what both must meet.

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

#include "dsp.hpp"
#include "../harness.hpp"

using namespace sigcore;
using cf = std::complex<float>;

namespace {

constexpr double kPi = 3.14159265358979323846;

// Unit-amplitude complex exponential exp(j*2*pi*bin*n/N).
std::vector<cf> tone(unsigned int n, double cycles_per_frame) {
    std::vector<cf> x(n);
    for (unsigned int i = 0; i < n; ++i) {
        const double ph = 2.0 * kPi * cycles_per_frame * i / n;
        x[i] = cf(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
    }
    return x;
}

// O(N^2) reference DFT in double precision.
std::vector<std::complex<double>> naive_dft(const std::vector<cf>& x) {
    const size_t n = x.size();
    std::vector<std::complex<double>> y(n);
    for (size_t k = 0; k < n; ++k) {
        std::complex<double> acc = 0;
        for (size_t t = 0; t < n; ++t) {
            const double ph = -2.0 * kPi * static_cast<double>(k * t % n) / n;
            acc += std::complex<double>(x[t]) * std::complex<double>(std::cos(ph), std::sin(ph));
        }
        y[k] = acc;
    }
    return y;
}

size_t argmax(const std::vector<float>& v) {
    size_t best = 0;
    for (size_t i = 1; i < v.size(); ++i)
        if (v[i] > v[best]) best = i;
    return best;
}

size_t argmax_mag(const std::vector<cf>& v) {
    size_t best = 0;
    for (size_t i = 1; i < v.size(); ++i)
        if (std::abs(v[i]) > std::abs(v[best])) best = i;
    return best;
}

}  // namespace

TEST_CASE("fft of an on-bin tone is a single peak of magnitude N (power of two)") {
    const unsigned int n = 256, bin = 37;
    const auto y = fft_forward(tone(n, bin));
    CHECK(argmax_mag(y) == bin);
    CHECK_NEAR(std::abs(y[bin]), n, 1e-2);
    for (unsigned int k = 0; k < n; ++k)
        if (k != bin) CHECK(std::abs(y[k]) < 1e-2);
}

TEST_CASE("fft matches a reference DFT for a power-of-two size") {
    std::vector<cf> x(128);
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = cf(std::sin(0.3f * i) + 0.5f * std::cos(1.7f * i), std::cos(0.11f * i * i));
    const auto y = fft_forward(x);
    const auto ref = naive_dft(x);
    for (size_t k = 0; k < x.size(); ++k) {
        CHECK_NEAR(y[k].real(), ref[k].real(), 5e-3);
        CHECK_NEAR(y[k].imag(), ref[k].imag(), 5e-3);
    }
}

TEST_CASE("fft matches a reference DFT for non-power-of-two sizes (mixed radix / Rader)") {
    // 100 = 2^2 * 5^2 (mixed radix), 97 is prime (Rader), 1000 = 2^3 * 5^3.
    for (unsigned int n : {100u, 97u, 1000u}) {
        std::vector<cf> x(n);
        for (unsigned int i = 0; i < n; ++i)
            x[i] = cf(std::sin(0.21f * i) + 0.25f * std::cos(2.9f * i), std::sin(0.05f * i * i));
        const auto y = fft_forward(x);
        const auto ref = naive_dft(x);
        for (unsigned int k = 0; k < n; ++k) {
            CHECK_NEAR(y[k].real(), ref[k].real(), 2e-2);
            CHECK_NEAR(y[k].imag(), ref[k].imag(), 2e-2);
        }
    }
}

TEST_CASE("fft is linear and preserves energy (Parseval)") {
    const auto a = tone(512, 20), b = tone(512, 111);
    std::vector<cf> sum(512);
    for (size_t i = 0; i < sum.size(); ++i) sum[i] = a[i] + 2.0f * b[i];
    const auto y = fft_forward(sum);
    double time_energy = 0, freq_energy = 0;
    for (const auto& v : sum) time_energy += std::norm(v);
    for (const auto& v : y) freq_energy += std::norm(v);
    CHECK_NEAR(freq_energy / 512.0, time_energy, 1e-1);
    CHECK_NEAR(std::abs(y[20]), 512.0, 1e-1);
    CHECK_NEAR(std::abs(y[111]), 1024.0, 1e-1);
}

TEST_CASE("psd of a tone peaks at the fft-shifted bin") {
    // Tone at +0.25*fs: shifted index = nfft/2 + 0.25*nfft.
    const unsigned int nfft = 512;
    const auto x = tone(8192, 8192 * 0.25);
    const auto psd = compute_psd(x, nfft);
    CHECK(psd.size() == nfft);
    CHECK(argmax(psd) == nfft / 2 + nfft / 4);
    // Well above the noise floor: peak at least 40 dB over the median bin.
    std::vector<float> sorted(psd);
    std::sort(sorted.begin(), sorted.end());
    CHECK(psd[argmax(psd)] - sorted[sorted.size() / 2] > 40.0f);
}

TEST_CASE("psd of a negative-frequency tone lands below DC") {
    const unsigned int nfft = 256;
    const auto x = tone(4096, -4096 * 0.125);
    const auto psd = compute_psd(x, nfft);
    CHECK(argmax(psd) == nfft / 2 - nfft / 8);
}

TEST_CASE("spectrogram tracks a frequency step between frames") {
    const unsigned int nfft = 256, hop = 256, half = 2048;
    auto x = tone(half, half * 0.10);  // +0.10 fs
    const auto second = tone(half, -static_cast<double>(half) * 0.20);  // -0.20 fs
    x.insert(x.end(), second.begin(), second.end());

    unsigned int frames = 0;
    const auto s = compute_spectrogram(x, nfft, hop, &frames);
    CHECK(frames == (x.size() - nfft) / hop + 1);
    CHECK(s.size() == static_cast<size_t>(frames) * nfft);

    auto row_peak = [&](unsigned int f) {
        const std::vector<float> row(s.begin() + static_cast<long>(f) * nfft,
                                     s.begin() + static_cast<long>(f + 1) * nfft);
        return static_cast<int>(argmax(row));
    };
    // First frame: +0.10 fs -> bin nfft/2 + 0.10*nfft ~ 153; last frame: -0.20 fs -> ~77.
    CHECK(std::abs(row_peak(0) - static_cast<int>(nfft / 2 + std::lround(0.10 * nfft))) <= 1);
    CHECK(std::abs(row_peak(frames - 1) - static_cast<int>(nfft / 2 - std::lround(0.20 * nfft))) <= 1);
}

TEST_CASE("lowpass filter passes an in-band tone and rejects an out-of-band one") {
    // On-bin tones (82/4096 ~ 0.02 fs, 1638/4096 ~ 0.40 fs) so nothing leaks
    // across FFT bins. Cutoff fc = 0.10 fs.
    const unsigned int n = 4096, lo_bin = 82, hi_bin = 1638;
    const float fc = 0.10f;
    const auto lo = tone(n, lo_bin), hi = tone(n, hi_bin);
    std::vector<cf> mix(n);
    for (unsigned int i = 0; i < n; ++i) mix[i] = lo[i] + hi[i];

    const auto y = lowpass_filter(mix, 101, fc, 60.0f);
    CHECK(y.size() == n);
    const auto spec = fft_forward(y);
    const double pass = std::abs(spec[lo_bin]) / n;
    const double stop = std::abs(spec[hi_bin]) / n;
    // lowpass_filter keeps liquid's raw Kaiser taps (set_scale(1.0)), whose DC
    // gain is 1/(2*fc) = 5; the ~100-sample start-up transient costs a few %.
    CHECK_NEAR(pass, 1.0 / (2.0 * fc), 0.4);
    CHECK(stop < pass * 1e-3);  // > 60 dB down out of band
}

TEST_CASE("empty and undersized inputs are rejected") {
    bool threw = false;
    try { fft_forward({}); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    threw = false;
    unsigned int frames = 0;
    try { compute_spectrogram(tone(64, 1), 256, 64, &frames); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
}

TEST_MAIN()
