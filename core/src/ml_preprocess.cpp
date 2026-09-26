#include "ml_preprocess.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "dsp.hpp"

namespace sigcore {

std::vector<float> make_spectrogram_image(const std::vector<std::complex<float>>& samples) {
    const size_t n = samples.size();
    if (n < kMlNfft) {
        throw std::runtime_error("ml preprocess: need at least " + std::to_string(kMlNfft) +
                                 " samples, got " + std::to_string(n));
    }

    const unsigned int hop =
        std::max(1u, static_cast<unsigned int>((n - kMlNfft) / std::max(1u, kMlFrames - 1)));
    unsigned int frames = 0;
    const std::vector<float> spec = compute_spectrogram(samples, kMlNfft, hop, &frames);  // frames x nfft

    // Transpose to (freq bin, frame); a short capture repeats its last frame
    // (numpy "edge" padding), a long one is cut to the first kMlFrames.
    std::vector<float> image(static_cast<size_t>(kMlNfft) * kMlFrames);
    for (unsigned int f = 0; f < kMlFrames; ++f) {
        const unsigned int src = std::min(f, frames - 1);
        for (unsigned int k = 0; k < kMlNfft; ++k) {
            image[static_cast<size_t>(k) * kMlFrames + f] = spec[static_cast<size_t>(src) * kMlNfft + k];
        }
    }

    // Population mean/std (numpy's defaults), accumulated in double.
    double sum = 0.0;
    for (float v : image) sum += v;
    const double mean = sum / static_cast<double>(image.size());
    double sq = 0.0;
    for (float v : image) {
        const double d = v - mean;
        sq += d * d;
    }
    const double stddev = std::sqrt(sq / static_cast<double>(image.size()));

    if (stddev > 1e-6) {
        for (float& v : image) v = static_cast<float>((v - mean) / stddev);
    } else {
        for (float& v : image) v = static_cast<float>(v - mean);
    }
    return image;
}

}  // namespace sigcore
