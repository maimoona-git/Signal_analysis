#pragma once

// Feature 3 (SIH26147 extra feature): soft-decision demodulation. Instead of forcing
// each received symbol to the nearest constellation point, this reports a
// log-likelihood ratio (LLR) per bit: how confident the demodulator is, not just its
// best guess. De-interleaving (interleave.hpp's float overloads) and FEC decoding
// (soft_fec.hpp) consume these directly.
//
// Convention (kept the same everywhere in this port): LLR = log(P(bit=0)/P(bit=1)).
// Positive means the bit is probably 0; the magnitude is the confidence. Hard decision
// from an LLR is llr_hard_decision below (bit = (llr < 0) ? 1 : 0), matching that sign.

#include <complex>
#include <cstdint>
#include <vector>

#include "demod.hpp"

namespace sigcore {

using LLRBuffer = std::vector<float>;

// Sign of the LLR only: the hard-decision path uses exactly this on the same LLRs the
// soft path computed, so hard vs soft differ only in the confidence information carried
// between stages, never in the underlying likelihoods themselves.
std::vector<uint8_t> llr_hard_decision(const LLRBuffer& llrs);

// Per-real-dimension AWGN noise variance (sigma^2), estimated decision-directed: hard-
// demodulate, re-modulate the ideal symbols, and take half the mean squared error
// against the received samples (each of Re/Im carries half of a complex sample's
// error energy under AWGN). Throws std::runtime_error on empty input, exactly like
// linear_demodulate. Never returns exactly 0 (a floor keeps later LLRs finite even for
// a noiseless capture).
float estimate_noise_variance_linear(const std::vector<std::complex<float>>& samples, ModulationScheme scheme);

// One LLR per bit, `bits_per_symbol(scheme)` LLRs per sample, in the same bit order
// linear_demodulate's hard bits are in. Max-log approximation over the full
// constellation: LLR_b = (min_{bit_b=1} |y-s|^2 - min_{bit_b=0} |y-s|^2) / (2*sigma2).
// This is the exact BPSK/QPSK closed form in the two-point/independent-axis cases and
// the standard approximation for 8-PSK/QAM and higher; one implementation covers every
// linear scheme instead of a separate closed form per constellation.
LLRBuffer soft_demodulate_linear(const std::vector<std::complex<float>>& samples, ModulationScheme scheme,
                                 float sigma2);

// NOT IMPLEMENTED for FSK, on purpose: liquid's fskmod is phase-continuous across
// symbols (each symbol's waveform depends on where the previous one left the phase),
// so a reference tone generated for one symbol in isolation is not what that symbol
// looks like inside a real sequence -- confirmed by measurement (the true transmitted
// symbol's isolated-tone distance was not even the smallest of the four candidates on
// a two-symbol test block). A correct soft metric would need to re-synthesise the
// whole downstream waveform per hypothesis, which is real additional work, not a small
// fix, so FSK stays hard-decision only for now; soft demodulation covers every linear
// scheme (BPSK/PSK/QAM/ASK), which is what Viterbi/LDPC are paired with in practice.

}  // namespace sigcore
