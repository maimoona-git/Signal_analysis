#pragma once

#include <string>

#include "signal_data.hpp"

namespace sigcore {

// Loads a raw .IQ file: interleaved 32-bit float complex samples
// (I, Q, I, Q, ...), no header — matches GNU Radio's default complex64
// file sink format. Sample rate is not embedded in the file and must be
// supplied by the caller.
SignalData load_iq(const std::string& path, double sample_rate);

}  // namespace sigcore
