#pragma once

// Port of app/io.py: turns a capture into samples + metadata. The browser hands
// over bytes and a filename (there is no path to open), so this takes those; the
// format rules are the original's.

#include <complex>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sigapp {

constexpr double kDefaultIqSampleRate = 1'000'000.0;  // Hz, for raw .IQ files given no rate

struct LoadedSignal {
    std::vector<std::complex<float>> samples;
    double sample_rate = 0.0;
    std::string format;
    size_t num_samples = 0;
    double duration_sec = 0.0;
    std::string path;  // the filename as given
};

// Format is chosen by the filename extension, exactly as the original does:
// ".wav" -> WAV (sample rate from the header, `sample_rate` ignored), ".iq" ->
// raw complex64 with `sample_rate`, or kDefaultIqSampleRate if it is absent or 0;
// anything else throws std::invalid_argument. (sigcore::ingest_bytes, used by
// the bridge's plain ingest, is deliberately more lenient: it sniffs content.)
LoadedSignal load_signal(const uint8_t* data, size_t size, const std::string& filename,
                         std::optional<double> sample_rate = std::nullopt);

}  // namespace sigapp
