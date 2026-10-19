#include "dsp.hpp"

#include <complex>

#include <liquid/liquid.h>

#include <cmath>
#include <stdexcept>

namespace sigcore {

namespace {

liquid_float_complex* as_liquid(std::complex<float>* p) {
    return reinterpret_cast<liquid_float_complex*>(p);
}

const liquid_float_complex* as_liquid(const std::complex<float>* p) {
    return reinterpret_cast<const liquid_float_complex*>(p);
}

// ---- Bluestein ---------------------------------------------------------------------------
// liquid's mixed-radix FFT handles a length like 97,920 = 2^7 x 3^2 x 5 x 17 in milliseconds but
// falls back to an O(n^2) transform when a large prime factor is left: 100,003 samples took 17 s.
// Real captures have arbitrary lengths (found with a real recording whose 97,919 samples cost
// 16 s inside carrier recovery). Bluestein's chirp-z identity turns any length into three
// power-of-two FFTs, in double precision so the chirp phase stays accurate.

using cd = std::complex<double>;

void fft_pow2(std::vector<cd>& a, bool inverse) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = 2.0 * M_PI / static_cast<double>(len) * (inverse ? 1.0 : -1.0);
        const cd wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            cd w(1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k) {
                const cd u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse) for (auto& v : a) v /= static_cast<double>(n);
}

std::vector<std::complex<float>> fft_bluestein(const std::vector<std::complex<float>>& x) {
    const size_t n = x.size();
    size_t m = 1;
    while (m < 2 * n - 1) m <<= 1;
    // chirp w_k = exp(-i pi k^2 / n); k^2 is reduced mod 2n first so the angle stays small
    std::vector<cd> w(n);
    for (size_t k = 0; k < n; ++k) {
        const unsigned long long k2 = (static_cast<unsigned long long>(k) * k) % (2ull * n);
        const double ang = -M_PI * static_cast<double>(k2) / static_cast<double>(n);
        w[k] = cd(std::cos(ang), std::sin(ang));
    }
    std::vector<cd> a(m, cd(0, 0)), b(m, cd(0, 0));
    for (size_t k = 0; k < n; ++k) a[k] = cd(x[k].real(), x[k].imag()) * w[k];
    b[0] = std::conj(w[0]);
    for (size_t k = 1; k < n; ++k) b[k] = b[m - k] = std::conj(w[k]);
    fft_pow2(a, false);
    fft_pow2(b, false);
    for (size_t i = 0; i < m; ++i) a[i] *= b[i];
    fft_pow2(a, true);
    std::vector<std::complex<float>> y(n);
    for (size_t k = 0; k < n; ++k) {
        const cd v = a[k] * w[k];
        y[k] = std::complex<float>(static_cast<float>(v.real()), static_cast<float>(v.imag()));
    }
    return y;
}

size_t largest_prime_factor(size_t n) {
    size_t largest = 1;
    for (size_t p = 2; p * p <= n; ++p) {
        while (n % p == 0) { largest = p; n /= p; }
    }
    return n > 1 ? n : largest;
}

}  // namespace

std::vector<std::complex<float>> fft_forward(const std::vector<std::complex<float>>& x) {
    if (x.empty()) {
        throw std::runtime_error("fft_forward: input signal is empty");
    }
    // cost of liquid's fallback is about n x (largest prime factor): switch before it hurts
    if (x.size() > 2048 && static_cast<double>(x.size()) * static_cast<double>(largest_prime_factor(x.size())) > 4.0e6) {
        return fft_bluestein(x);
    }
    std::vector<std::complex<float>> y(x.size());
    fft_run(static_cast<unsigned int>(x.size()), const_cast<liquid_float_complex*>(as_liquid(x.data())),
            as_liquid(y.data()), LIQUID_FFT_FORWARD, 0);
    return y;
}

std::vector<float> compute_psd(const std::vector<std::complex<float>>& x, unsigned int nfft) {
    if (x.empty()) {
        throw std::runtime_error("compute_psd: input signal is empty");
    }
    if (nfft < 2) {
        throw std::runtime_error("compute_psd: nfft must be >= 2");
    }

    std::vector<float> psd(nfft);
    spgramcf_estimate_psd(nfft, const_cast<liquid_float_complex*>(as_liquid(x.data())),
                           static_cast<unsigned int>(x.size()), psd.data());
    return psd;
}

std::vector<float> compute_spectrogram(const std::vector<std::complex<float>>& x,
                                        unsigned int nfft, unsigned int hop,
                                        unsigned int* out_num_frames) {
    if (x.empty()) {
        throw std::runtime_error("compute_spectrogram: input signal is empty");
    }
    if (nfft < 2) {
        throw std::runtime_error("compute_spectrogram: nfft must be >= 2");
    }
    if (hop == 0) {
        throw std::runtime_error("compute_spectrogram: hop must be > 0");
    }
    if (x.size() < nfft) {
        throw std::runtime_error("compute_spectrogram: signal shorter than nfft");
    }

    const size_t n = x.size();
    const unsigned int num_frames = static_cast<unsigned int>((n - nfft) / hop + 1);
    *out_num_frames = num_frames;

    std::vector<float> result(static_cast<size_t>(num_frames) * nfft);
    std::vector<std::complex<float>> windowed(nfft);
    std::vector<std::complex<float>> spectrum(nfft);

    for (unsigned int frame = 0; frame < num_frames; ++frame) {
        const size_t start = static_cast<size_t>(frame) * hop;
        for (unsigned int i = 0; i < nfft; ++i) {
            // Hamming window
            float w = 0.54f - 0.46f * std::cos(2.0f * static_cast<float>(M_PI) * i /
                                                (nfft - 1));
            windowed[i] = x[start + i] * w;
        }

        fft_run(nfft, as_liquid(windowed.data()), as_liquid(spectrum.data()), LIQUID_FFT_FORWARD, 0);
        fft_shift(as_liquid(spectrum.data()), nfft);

        float* row = result.data() + static_cast<size_t>(frame) * nfft;
        for (unsigned int i = 0; i < nfft; ++i) {
            float mag = std::abs(spectrum[i]);
            row[i] = 20.0f * std::log10(mag + 1e-12f);
        }
    }

    return result;
}

std::vector<std::complex<float>> lowpass_filter(const std::vector<std::complex<float>>& x,
                                                 unsigned int n_taps, float fc, float as_db) {
    if (x.empty()) {
        throw std::runtime_error("lowpass_filter: input signal is empty");
    }
    if (n_taps == 0) {
        throw std::runtime_error("lowpass_filter: n_taps must be > 0");
    }
    if (!(fc > 0.0f && fc < 0.5f)) {
        throw std::runtime_error("lowpass_filter: fc must satisfy 0 < fc < 0.5");
    }

    firfilt_crcf filt = firfilt_crcf_create_kaiser(n_taps, fc, as_db, 0.0f);
    firfilt_crcf_set_scale(filt, 1.0f);

    std::vector<std::complex<float>> y(x.size());
    firfilt_crcf_execute_block(filt, const_cast<liquid_float_complex*>(as_liquid(x.data())),
                                static_cast<unsigned int>(x.size()), as_liquid(y.data()));

    firfilt_crcf_destroy(filt);
    return y;
}

}  // namespace sigcore
