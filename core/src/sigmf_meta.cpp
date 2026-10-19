#include "sigmf_meta.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace sigcore {

namespace {

// Position just after `"key":` (whitespace skipped), or npos.
size_t value_pos(const std::string& json, const std::string& key, size_t from = 0) {
    const std::string needle = "\"" + key + "\"";
    size_t p = json.find(needle, from);
    if (p == std::string::npos) return p;
    p = json.find(':', p + needle.size());
    if (p == std::string::npos) return p;
    ++p;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\n' || json[p] == '\r')) ++p;
    return p < json.size() ? p : std::string::npos;
}

std::optional<double> number_after(const std::string& json, size_t p) {
    if (p == std::string::npos) return std::nullopt;
    const char* start = json.c_str() + p;
    char* end = nullptr;
    const double v = std::strtod(start, &end);
    if (end == start || !std::isfinite(v)) return std::nullopt;
    return v;
}

}  // namespace

SigmfMeta parse_sigmf_meta(const std::string& json) {
    SigmfMeta m;
    m.sample_rate = number_after(json, value_pos(json, "core:sample_rate"));
    if (m.sample_rate && *m.sample_rate <= 0.0) m.sample_rate.reset();
    const size_t captures = json.find("\"captures\"");
    if (captures != std::string::npos) m.center_freq_hz = number_after(json, value_pos(json, "core:frequency", captures));
    const size_t dt = value_pos(json, "core:datatype");
    if (dt != std::string::npos && json[dt] == '"') {
        const size_t end = json.find('"', dt + 1);
        if (end != std::string::npos) {
            const std::string t = json.substr(dt + 1, end - dt - 1);
            if (t == "cf32_le" || t == "cf32") m.format = IqSampleFormat::Cf32;
            else if (t == "ci16_le" || t == "ci16") m.format = IqSampleFormat::Ci16;
            else if (t == "cu8") m.format = IqSampleFormat::Cu8;
            else if (t == "ri16_le" || t == "rf32_le") {
                // REAL samples. Found on the real SigMF logo capture: 2 channels of real int16,
                // which is a stereo recording used as I and Q. A two-channel real file read as
                // I/Q is the same interleaving as the complex type of the same width -- the
                // same call the WAV path makes for a stereo .wav. One channel is genuinely real
                // data this tool cannot demodulate as I/Q, so it is left unrecognised.
                const auto ch = number_after(json, value_pos(json, "core:num_channels"));
                if (ch && *ch == 2.0) m.format = (t == "ri16_le") ? IqSampleFormat::Ci16 : IqSampleFormat::Cf32;
            }
        }
    }
    return m;
}

IqSampleFormat guess_iq_format(const uint8_t* data, size_t size) {
    const size_t words = size / 4;
    if (words >= 16) {
        size_t sane = 0, nonzero = 0;
        bool seen[256] = {false};
        size_t distinct = 0;
        for (size_t i = 0; i < words; ++i) {
            float f;
            std::memcpy(&f, data + 4 * i, 4);
            if (!seen[data[4 * i + 3]]) { seen[data[4 * i + 3]] = true; ++distinct; }
            if (std::isfinite(f) && std::fabs(f) < 1000.0f) {
                ++sane;
                if (std::fabs(f) > 1e-6f) ++nonzero;
            }
        }
        // Float sign/exponent bytes take few distinct values for signals of bounded level;
        // int16 pairs reread as floats scatter across many (the Q sample's high byte).
        if (sane * 100 >= words * 99 && nonzero * 2 >= words && distinct <= 40) return IqSampleFormat::Cf32;
    }
    // uint8 I/Q is centred on ~127.5, so essentially every byte sits mid-range; int16
    // I/Q has uniformly spread low bytes, which a mid-range test rejects.
    size_t mid = 0;
    for (size_t i = 0; i < size; ++i) mid += (data[i] >= 64 && data[i] <= 192);
    if (size && mid * 10 >= size * 9) return IqSampleFormat::Cu8;
    return IqSampleFormat::Ci16;
}

}  // namespace sigcore
