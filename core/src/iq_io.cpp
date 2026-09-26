#include "iq_io.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace sigcore {

SignalData parse_iq(const uint8_t* data, size_t size, double sample_rate,
                    const std::string& source) {
    if (sample_rate <= 0.0) {
        throw std::runtime_error("iq: sample_rate must be positive: " + source);
    }

    constexpr size_t kBytesPerSample = 2 * sizeof(float);  // I + Q, float32 each
    if (size % kBytesPerSample != 0) {
        throw std::runtime_error("iq: file size not aligned to complex64 sample size: " + source);
    }

    const size_t num_samples = size / kBytesPerSample;

    SignalData sig;
    sig.sample_rate = sample_rate;
    sig.format = "iq_cf32";
    sig.samples.resize(num_samples);

    for (size_t i = 0; i < num_samples; ++i) {
        float re, im;
        std::memcpy(&re, data + i * kBytesPerSample, sizeof(float));
        std::memcpy(&im, data + i * kBytesPerSample + sizeof(float), sizeof(float));
        sig.samples[i] = std::complex<float>(re, im);
    }

    return sig;
}

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
    if (size < 0) {
        throw std::runtime_error("iq: could not determine file size: " + path);
    }

    std::vector<uint8_t> raw(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(raw.data()), size);
    if (static_cast<std::streamsize>(f.gcount()) != size) {
        throw std::runtime_error("iq: failed to read full file: " + path);
    }

    return parse_iq(raw.data(), raw.size(), sample_rate, path);
}

}  // namespace sigcore
