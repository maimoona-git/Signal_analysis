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

}  // namespace

std::vector<std::complex<float>> fft_forward(const std::vector<std::complex<float>>& x) {
    if (x.empty()) {
        throw std::runtime_error("fft_forward: input signal is empty");
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
