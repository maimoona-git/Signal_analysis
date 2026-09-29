#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "signal_data.hpp"

namespace sigcore {

// How a raw .IQ (or SigMF .sigmf-data) file's samples are stored on disk. Matches
// SigMF's own "core:datatype" naming (cf32_le / ci16_le / cu8), since that is the
// vocabulary a file's own metadata sidecar (when there is one) already uses.
//  Cf32: interleaved 32-bit float I/Q, [-1, 1]-ish, no scaling needed -- GNU Radio's
//        default complex64 file sink format, and this project's own native convention.
//  Ci16: interleaved 16-bit signed integer I/Q, full-scale +-32768; divided by 32768
//        to land in [-1, 1] -- common for USRP and other higher-resolution SDRs.
//  Cu8:  interleaved 8-bit UNSIGNED integer I/Q, centred on 127.5 (not 0) rather than
//        signed around zero; RTL-SDR's own native format, arguably the single most
//        common raw capture format in the hobbyist/cheap-SDR world.
enum class IqSampleFormat { Cf32, Ci16, Cu8 };

// Parses raw .IQ bytes: interleaved complex samples in `format` (I, Q, I, Q, ...), no
// header. Sample rate is not embedded in the file and must be supplied by the caller.
// `source` only labels error messages.
SignalData parse_iq(const uint8_t* data, size_t size, double sample_rate,
                    const std::string& source = "<memory>", IqSampleFormat format = IqSampleFormat::Cf32);

// Loads a raw .IQ file from disk: reads it whole and calls parse_iq.
SignalData load_iq(const std::string& path, double sample_rate, IqSampleFormat format = IqSampleFormat::Cf32);

}  // namespace sigcore
