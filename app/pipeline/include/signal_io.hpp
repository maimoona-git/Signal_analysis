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

#include "iq_io.hpp"

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

// Format is chosen by the filename extension, exactly as the original does, plus one
// deliberate addition: ".wav" -> WAV (sample rate from the header, `sample_rate`
// ignored); ".iq" or ".sigmf-data" -> raw I/Q with `sample_rate` (or kDefaultIqSampleRate
// if it is absent or 0), read in `iq_format` (Cf32 unless told otherwise -- see
// sigcore::IqSampleFormat's own comment for what the other two are and when a real
// capture actually uses them); anything else throws std::invalid_argument.
// ".sigmf-data" is accepted because its bytes, for the (default, most common) Cf32
// case, ARE a raw .IQ file byte for byte -- SigMF's own ".sigmf-meta" JSON sidecar,
// if a caller has one, is not parsed here; this only spares a user the rename.
// (sigcore::ingest_bytes, used by the bridge's plain ingest, is deliberately more
// lenient: it sniffs content instead of trusting the extension.)
LoadedSignal load_signal(const uint8_t* data, size_t size, const std::string& filename,
                         std::optional<double> sample_rate = std::nullopt,
                         sigcore::IqSampleFormat iq_format = sigcore::IqSampleFormat::Cf32);

}  // namespace sigapp
