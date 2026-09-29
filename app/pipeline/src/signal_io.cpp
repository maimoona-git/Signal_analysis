#include "signal_io.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include "iq_io.hpp"
#include "wav_io.hpp"

namespace sigapp {

namespace {

// Python's Path.suffix: from the last '.' of the final path component, but
// not for a leading dot (".wav" alone has no suffix) or a trailing dot.
std::string path_suffix(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == name.size()) return "";
    std::string suffix = name.substr(dot);
    std::transform(suffix.begin(), suffix.end(), suffix.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return suffix;
}

}  // namespace

LoadedSignal load_signal(const uint8_t* data, size_t size, const std::string& filename,
                         std::optional<double> sample_rate, sigcore::IqSampleFormat iq_format) {
    const std::string suffix = path_suffix(filename);

    sigcore::SignalData raw;
    if (suffix == ".wav") {
        raw = sigcore::parse_wav(data, size, filename);
    } else if (suffix == ".iq" || suffix == ".sigmf-data") {
        const double rate = (sample_rate.has_value() && *sample_rate != 0.0) ? *sample_rate : kDefaultIqSampleRate;
        raw = sigcore::parse_iq(data, size, rate, filename, iq_format);
    } else {
        throw std::invalid_argument("unsupported file extension: '" + suffix +
                                    "' (expected .wav, .iq or .sigmf-data)");
    }

    LoadedSignal out;
    out.sample_rate = raw.sample_rate;
    out.format = raw.format;
    out.num_samples = raw.num_samples();
    out.duration_sec = raw.duration_sec();
    out.samples = std::move(raw.samples);
    out.path = filename;
    return out;
}

}  // namespace sigapp
