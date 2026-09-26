#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "signal_data.hpp"

namespace sigcore {

// Parses raw .IQ bytes: interleaved 32-bit float complex samples
// (I, Q, I, Q, ...), no header — matches GNU Radio's default complex64
// file sink format. Sample rate is not embedded in the file and must be
// supplied by the caller. `source` only labels error messages.
SignalData parse_iq(const uint8_t* data, size_t size, double sample_rate,
                    const std::string& source = "<memory>");

// Loads a raw .IQ file from disk: reads it whole and calls parse_iq.
SignalData load_iq(const std::string& path, double sample_rate);

}  // namespace sigcore
