#pragma once

#include <string>

#include "signal_data.hpp"

namespace sigcore {

// Loads a RIFF/WAVE file. Mono files are treated as real-valued signals
// (imaginary part zero); stereo files are treated as interleaved I/Q
// (left channel = I, right channel = Q). Supports 16-bit PCM and 32-bit
// IEEE float samples.
SignalData load_wav(const std::string& path);

}  // namespace sigcore
