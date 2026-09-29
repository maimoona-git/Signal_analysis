#pragma once

#include <complex>
#include <vector>

#include "demod.hpp"

namespace sigcore {

// Carrier and timing recovery for linear modulations (PSK/QAM/ASK/OOK), for a real
// capture: samples that are oversampled at (approximately) `sps` samples per symbol
// but, unlike this project's own synthetic captures, are not already phase-aligned
// to the symbol clock and are not already frequency/phase-locked to the carrier.
// `linear_demodulate`'s nearest-symbol slicing assumes both are already true; naive
// decimation (take every `sps`-th sample) only actually gives that on a synthetic
// capture built with zero timing offset and zero carrier error.
//
// Not attempted here: blind estimation of `sps` itself (the caller must supply an
// approximate nominal rate -- already a required GUI/API input for every linear
// scheme); FSK (its own frequency-discriminator demodulation is far more tolerant
// of timing/carrier error than PSK/QAM's phase-sensitive slicing, and liquid's
// matched-filter symbol synchronizer used here is built for linear modulations,
// not FSK's tone bursts); and phase ambiguity (see RecoveryResult below).
struct RecoveryResult {
    std::vector<std::complex<float>> samples;  // ~1 sample/symbol, ready for linear_demodulate
    float coarse_cfo_cycles_per_sample = 0.0f; // the frequency offset found and removed, for diagnostics

    // PSK/QAM constellations are rotationally symmetric (BPSK: 180 degrees, QPSK:
    // 90 degrees, and so on), so a decision-directed carrier recovery loop can lock
    // onto any of that many equally-valid rotations -- there is nothing in the
    // signal itself that says which one is "upright" without a known preamble or
    // differentially-encoded bits, neither of which this project's demod layer has.
    // `samples` is therefore only correct up to one of `phase_ambiguity_count` equally
    // likely constant phase rotations; the caller sees a real message's bits scrambled
    // by a fixed (but unknown) rotation exactly as often as it sees them clean. This
    // is a known, documented limitation, not a bug -- resolving it needs either a sync
    // word to test rotations against (the way param_search.cpp already tests FEC/
    // interleaver candidates by re-encode agreement) or differential coding, both out
    // of scope for this pass.
    unsigned int phase_ambiguity_count = 1;
};

// Throws std::runtime_error if `raw` is empty or `sps < 2` (nothing to recover: at
// sps <= 1 there is no timing offset to correct).
RecoveryResult recover_linear(const std::vector<std::complex<float>>& raw, unsigned int sps,
                              ModulationScheme scheme);

}  // namespace sigcore
