#pragma once

#include <complex>
#include <string>
#include <vector>

namespace sigcore {

// A blind check on the classifier's most common real-world mistake. The shipped spectrogram
// classifier called every one of ten REAL BPSK recordings "fsk" (90-99% confidence); a spectrogram
// cannot tell them apart reliably, but three cheap measurements on the raw samples can:
//   1. the blind FSK estimator (fsk_estimate.hpp): real FSK shows clean tone clusters;
//   2. the envelope: FSK is constant-envelope, a pulse-shaped linear modulation is not;
//   3. the M-th power spectral line: raising PSK to the 2nd (BPSK, ASK, OOK) or 4th (QPSK) power
//      collapses its modulation to one spectral line, FSK to none.
// Calibrated on 600 synthetic FSK signals (2- and 4-FSK, deviation 0.04-0.28 cycles/sample, 5-30
// dB SNR: the envelope variation never exceeded 0.19, so none was ever flagged) and 10 real BPSK
// recordings (envelope variation 0.55-0.84, 2nd-power line 0.56-0.73, all flagged).
struct BlindLinearCheck {
    // "bpsk" (also what ASK/OOK read as: the same 2-fold symmetry), "qpsk", "psk8", or empty when
    // the check is not conclusive -- it only speaks when all three tests agree it is not FSK.
    std::string family;
    double fsk_confidence = 0.0;   // estimate_fsk_params' confidence (0 if it could not run)
    double envelope_cv = 0.0;      // std/mean of |x| over the strongest 60% of samples
    double line2 = 0.0, line4 = 0.0, line8 = 0.0;  // share of the k-th power spectrum in its peak
};

// Looks at (up to) the first 131,072 samples. Never throws; a too-short capture is inconclusive.
BlindLinearCheck blind_linear_check(const std::vector<std::complex<float>>& samples);

// Constellation check: the modulation order from the symbols themselves, which a spectrogram cannot
// show (BPSK/QPSK/8PSK and the QAM orders have near-identical spectra). Fourth-order cumulants of
// the unit-power symbol stream are fixed numbers per constellation and are unaffected by a carrier
// phase rotation (C40 is taken by magnitude):
//            |C40|   C42   E|a|^4
//   bpsk       2      -2    1
//   qpsk       1      -1    1
//   psk8       0      -1    1
//   qam16      0.68   -0.68 1.32
//   qam64      0.62   -0.62 1.38
// Gaussian noise adds nothing to a fourth-order cumulant but lowers the signal share S of the
// power, scaling both by S^2; S^2 is read from the fourth moment under each hypothesis
// (S^2 = (2 - m4) / (2 - E|a|^4)) and the hypothesis whose scaled cumulants fit best wins.
// Needs symbol-rate samples with the carrier offset removed (1 sample/symbol input, or the output
// of timing and carrier recovery). It cannot tell QAM16 from QAM64 on a short capture: the cumulant
// estimates carry a standard error of roughly 0.1 at a few hundred symbols, the two differ by 0.06.
struct ConstellationCheck {
    std::string best;       // "bpsk", "qpsk", "psk8", "qam16" or "qam64"; empty when not run
    double margin = 0.0;    // log-likelihood winner minus runner-up, per symbol (before the QAM gate)
    double qam_advantage = 0.0;  // best QAM ring structure's log-likelihood minus a single ring's, per symbol
    double c40 = 0.0;       // |C40| / C21^2 as measured
    double c42 = 0.0;       // C42 / C21^2 as measured
    std::vector<std::string> ranking;  // all five, best first
};

// Largest bin over mean of a Welch power spectrum (64-point Hann segments, up to the first 65,536
// samples). Linear modulation sampled at one sample per symbol is nearly white (measured 1.3 to 6.2
// over BPSK to QAM64, 3 to 30 dB), oversampled FSK has sharp tone lines (8.5 to 14), so this is the
// test of whether samples can be read as symbols at all. 0 for fewer than 128 samples.
double spectral_peakiness(const std::vector<std::complex<float>>& samples);

// Never throws; fewer than 100 symbols or an all-zero input leaves `best` empty.
ConstellationCheck constellation_check(const std::vector<std::complex<float>>& symbols);

}  // namespace sigcore
