#pragma once

#include <complex>
#include <vector>

namespace sigcore {

// Forward FFT of a complex signal (not fft-shifted), via liquid-dsp's fft_run.
std::vector<std::complex<float>> fft_forward(const std::vector<std::complex<float>>& x);

// Power spectral density estimate (dB, fft-shifted so index 0 is -fs/2),
// via liquid-dsp's spgramcf_estimate_psd. Output length is nfft.
std::vector<float> compute_psd(const std::vector<std::complex<float>>& x, unsigned int nfft);

// Short-time Fourier transform magnitude spectrogram (dB, fft-shifted per
// row). Returns a flattened row-major array of shape (num_frames, nfft);
// num_frames is written to *out_num_frames.
std::vector<float> compute_spectrogram(const std::vector<std::complex<float>>& x,
                                        unsigned int nfft, unsigned int hop,
                                        unsigned int* out_num_frames);

// Lowpass-filters a complex signal with a Kaiser-windowed FIR filter.
//  n_taps : filter length
//  fc     : normalized cutoff frequency, 0 < fc < 0.5
//  as_db  : stop-band attenuation in dB
std::vector<std::complex<float>> lowpass_filter(const std::vector<std::complex<float>>& x,
                                                 unsigned int n_taps, float fc, float as_db);

}  // namespace sigcore
