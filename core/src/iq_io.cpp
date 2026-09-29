#include "iq_io.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace sigcore {

namespace {

// Bytes per complex sample, and the on-disk format's own label (for SignalData::format
// and, via that, for the results panel and error messages), for each IqSampleFormat.
size_t bytes_per_sample(IqSampleFormat format) {
    switch (format) {
        case IqSampleFormat::Cf32: return 2 * sizeof(float);      // I + Q, float32 each
        case IqSampleFormat::Ci16: return 2 * sizeof(int16_t);    // I + Q, int16 each
        case IqSampleFormat::Cu8: return 2 * sizeof(uint8_t);     // I + Q, uint8 each
    }
    throw std::runtime_error("iq: unknown sample format");
}

const char* format_label(IqSampleFormat format) {
    switch (format) {
        case IqSampleFormat::Cf32: return "iq_cf32";
        case IqSampleFormat::Ci16: return "iq_ci16";
        case IqSampleFormat::Cu8: return "iq_cu8";
    }
    throw std::runtime_error("iq: unknown sample format");
}

// The on-disk sample-size word used in the "not aligned" error below, kept distinct
// per format so the message says what it actually expected -- but see error_text.hpp's
// own comment: the *shape* of this message ("iq: file size not aligned to ... sample
// size") is matched there across all three formats, not just this one's exact words.
const char* size_label(IqSampleFormat format) {
    switch (format) {
        case IqSampleFormat::Cf32: return "complex64";
        case IqSampleFormat::Ci16: return "complex-int16";
        case IqSampleFormat::Cu8: return "complex-uint8";
    }
    throw std::runtime_error("iq: unknown sample format");
}

std::complex<float> read_sample(const uint8_t* p, IqSampleFormat format) {
    switch (format) {
        case IqSampleFormat::Cf32: {
            float re, im;
            std::memcpy(&re, p, sizeof(float));
            std::memcpy(&im, p + sizeof(float), sizeof(float));
            return {re, im};
        }
        case IqSampleFormat::Ci16: {
            int16_t re, im;
            std::memcpy(&re, p, sizeof(int16_t));
            std::memcpy(&im, p + sizeof(int16_t), sizeof(int16_t));
            // Full-scale +-32768, not +-32767: matches SigMF's and GNU Radio's own
            // convention (the same asymmetry float audio codecs use for int16 PCM),
            // so 32767 (the largest positive sample) lands just under +1.0, not at it.
            return {static_cast<float>(re) / 32768.0f, static_cast<float>(im) / 32768.0f};
        }
        case IqSampleFormat::Cu8: {
            // Centred on 127.5, not 0 -- an unsigned format, so "no signal" is the
            // middle of the byte range, not the bottom of it. See the header's own
            // comment on why this (not just a narrower Ci16) needs its own conversion.
            const float re = (static_cast<float>(p[0]) - 127.5f) / 127.5f;
            const float im = (static_cast<float>(p[1]) - 127.5f) / 127.5f;
            return {re, im};
        }
    }
    throw std::runtime_error("iq: unknown sample format");
}

}  // namespace

SignalData parse_iq(const uint8_t* data, size_t size, double sample_rate,
                    const std::string& source, IqSampleFormat format) {
    if (sample_rate <= 0.0) {
        throw std::runtime_error("iq: sample_rate must be positive: " + source);
    }

    const size_t bytes = bytes_per_sample(format);
    if (size % bytes != 0) {
        throw std::runtime_error(std::string("iq: file size not aligned to ") + size_label(format) +
                                 " sample size: " + source);
    }

    const size_t num_samples = size / bytes;

    SignalData sig;
    sig.sample_rate = sample_rate;
    sig.format = format_label(format);
    sig.samples.resize(num_samples);

    for (size_t i = 0; i < num_samples; ++i) {
        sig.samples[i] = read_sample(data + i * bytes, format);
    }

    return sig;
}

SignalData load_iq(const std::string& path, double sample_rate, IqSampleFormat format) {
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

    return parse_iq(raw.data(), raw.size(), sample_rate, path, format);
}

}  // namespace sigcore
