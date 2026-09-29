#pragma once

#include <complex>
#include <vector>

namespace sigcore {

// Result of blind symbol-rate estimation: see estimate_symbol_rate's own
// comment for the technique and its measured limits.
struct SymbolRateEstimate {
    double samples_per_symbol = 0.0;  // estimated sps (sample_rate / estimated symbol rate), fractional
    double confidence = 0.0;          // 0..1: how much the found peak exceeds a noise-only floor (see .cpp)
};

// Blind estimate of samples-per-symbol from a raw, not-yet-demodulated capture
// -- no modulation, pulse-shape, or framing knowledge required. The caller
// still supplies a search range: `min_sps`/`max_sps` bound where the true
// value is assumed to lie (both must be >= 2, and min_sps < max_sps).
//
// Technique: a delay-and-multiply nonlinearity, |x[n] - x[n-1]|^2, spikes at
// every symbol boundary where consecutive symbols actually differ (unlike a
// plain |x[n]|^2 envelope, which is flat for any constant-modulus scheme --
// PSK/FSK/OOK's "1" state -- and so carries no periodicity to find at all).
// Over many symbols this nonlinearity's own spectrum shows a line at the true
// symbol rate -- plus, for an unshaped (rectangular-pulse) capture, strong
// energy at that line's harmonics too (measured, not assumed: see .cpp), so
// the search prefers the lowest-frequency bin among a tied-looking group
// rather than simply the single tallest one. The result is refined to
// sub-bin precision by a parabolic fit through its three tallest neighbouring
// bins (standard practice for FFT peak interpolation -- a magnitude
// spectrum's peak is locally well approximated by a parabola in
// log-magnitude near its true maximum).
//
// Throws std::runtime_error if `samples` has fewer than 256 points (not
// enough to average the estimate over multiple symbol periods) or if the
// sps bounds are invalid.
//
// Known limitation, measured while building this: an RRC-shaped capture at
// (or very near) the Nyquist minimum of 2 samples/symbol is a genuinely
// degenerate case for this technique (confirmed by this file's own test
// suite) -- confidence correctly reads ~0 there rather than reporting a
// wrong answer as if it were trustworthy, but the true rate is not found.
// Real captures essentially never run pulse-shaped linear modulation at
// exactly 2x oversampling in practice, so this was not chased further.
SymbolRateEstimate estimate_symbol_rate(const std::vector<std::complex<float>>& samples,
                                         double min_sps = 2.0, double max_sps = 64.0);

}  // namespace sigcore
