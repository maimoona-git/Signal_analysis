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

}  // namespace sigcore
