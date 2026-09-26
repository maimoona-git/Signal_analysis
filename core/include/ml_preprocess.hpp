#pragma once

#include <complex>
#include <vector>

namespace sigcore {

// Fixed input geometry of the modulation classifier CNN (ml/model.py).
constexpr unsigned int kMlNfft = 64;     // image rows: frequency bins
constexpr unsigned int kMlFrames = 48;   // image cols: time frames

// Turns a variable-length capture into the classifier's input image: a
// kMlNfft x kMlFrames row-major float array (row = frequency bin, column =
// time frame), z-score normalised. C++ port of ml/preprocessing.py's
// make_spectrogram_image; the two must agree (tests/data/ml holds fixtures
// produced by the original Python). Steps:
//   1. dB spectrogram with nfft = 64 and hop = max(1, (n - 64) / 47)
//   2. transpose so rows are frequency
//   3. keep the first 48 frames, or repeat the last frame to reach 48
//   4. (x - mean) / std over the whole image, or just x - mean if std <= 1e-6
// Throws std::runtime_error if the capture has fewer than kMlNfft samples.
std::vector<float> make_spectrogram_image(const std::vector<std::complex<float>>& samples);

}  // namespace sigcore
