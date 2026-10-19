#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "iq_io.hpp"

namespace sigcore {

// The few fields of a SigMF `.sigmf-meta` sidecar this tool uses. Absent fields stay
// unset; this is a tolerant scan, not a validating JSON parser, and never throws.
struct SigmfMeta {
    std::optional<double> sample_rate;      // global "core:sample_rate", Hz
    std::optional<double> center_freq_hz;   // first capture's "core:frequency", Hz
    std::optional<IqSampleFormat> format;   // from "core:datatype" (cf32_le, ci16_le, cu8)
};
SigmfMeta parse_sigmf_meta(const std::string& json);

// Gap-list Phase 2.7: best-effort guess of a raw capture's sample format from its bytes
// alone. Heuristic, not proof: float32 if nearly every 4-byte word is a finite float of
// plausible magnitude; otherwise uint8 if bytes cluster near 127.5, else int16.
IqSampleFormat guess_iq_format(const uint8_t* data, size_t size);

}  // namespace sigcore
