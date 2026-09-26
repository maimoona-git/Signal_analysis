#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "signal_data.hpp"

namespace sigcore {

// Parses a RIFF/WAVE file already in memory. Mono files are treated as
// real-valued signals (imaginary part zero); stereo files are treated as
// interleaved I/Q (left channel = I, right channel = Q). Supports 16-bit PCM,
// 8-bit PCM and 32-bit IEEE float samples.
//
// The input is untrusted (in the browser it is a user-selected file), so every
// chunk size is checked against the bytes actually present. A data chunk that
// claims more bytes than exist (a truncated or streamed file) is clamped to
// what is there, dropping any trailing partial frame.
//
// `source` only labels error messages (a path, or a filename from the browser).
SignalData parse_wav(const uint8_t* data, size_t size, const std::string& source = "<memory>");

// Loads a RIFF/WAVE file from disk: reads it whole and calls parse_wav.
SignalData load_wav(const std::string& path);

}  // namespace sigcore
