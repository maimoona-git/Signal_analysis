#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "signal_data.hpp"

namespace sigcore {

enum class FileKind { Wav, Iq };

// Decides how to parse an in-memory capture. A RIFF/WAVE header always means
// WAV, whatever the filename says. Otherwise a ".wav" name means WAV (so a
// mislabelled file fails as "not a valid RIFF/WAVE" instead of being read as
// noise), and anything else is treated as raw complex64 .IQ.
FileKind detect_file_kind(const uint8_t* data, size_t size, const std::string& filename);

// Parses a browser-provided capture. `sample_rate` is required (> 0) for raw
// .IQ, which has no header; it is ignored for WAV, which carries its own.
SignalData ingest_bytes(const uint8_t* data, size_t size, const std::string& filename,
                        double sample_rate);

}  // namespace sigcore
