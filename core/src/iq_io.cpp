#include "iq_io.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace sigcore {

SignalData load_iq(const std::string& path, double sample_rate) {
    if (sample_rate <= 0.0) {
        throw std::runtime_error("iq: sample_rate must be positive: " + path);
    }

    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        throw std::runtime_error("iq: could not open file: " + path);
    }

    std::streamsize size = f.tellg();
    f.seekg(0, std::ios::beg);

    constexpr size_t kBytesPerSample = 2 * sizeof(float);  // I + Q, float32 each
    if (size < 0 || static_cast<size_t>(size) % kBytesPerSample != 0) {
        throw std::runtime_error("iq: file size not aligned to complex64 sample size: " + path);
    }

    std::vector<char> raw(static_cast<size_t>(size));
    f.read(raw.data(), size);
    if (static_cast<std::streamsize>(f.gcount()) != size) {
        throw std::runtime_error("iq: failed to read full file: " + path);
    }

    const size_t num_samples = raw.size() / kBytesPerSample;

    SignalData sig;
    sig.sample_rate = sample_rate;
    sig.format = "iq_cf32";
    sig.samples.resize(num_samples);

    for (size_t i = 0; i < num_samples; ++i) {
        float re, im;
        std::memcpy(&re, raw.data() + i * kBytesPerSample, sizeof(float));
        std::memcpy(&im, raw.data() + i * kBytesPerSample + sizeof(float), sizeof(float));
        sig.samples[i] = std::complex<float>(re, im);
    }

    return sig;
}

}  // namespace sigcore
