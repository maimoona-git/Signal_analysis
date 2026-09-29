#include "recover.hpp"

#include <cmath>
#include <complex>
#include <stdexcept>

#include <liquid/liquid.h>

#include "dsp.hpp"

namespace sigcore {

namespace {

liquid_float_complex* as_liquid(std::complex<float>* p) {
    return reinterpret_cast<liquid_float_complex*>(p);
}

// Duplicated from demod.cpp's own (anonymous-namespace, so not reusable) mapping --
// see its comment for why FSK isn't here: this file never handles FSK.
modulation_scheme to_liquid_scheme(ModulationScheme scheme) {
    switch (scheme) {
        case ModulationScheme::BPSK: return LIQUID_MODEM_BPSK;
        case ModulationScheme::QPSK: return LIQUID_MODEM_QPSK;
        case ModulationScheme::PSK8: return LIQUID_MODEM_PSK8;
        case ModulationScheme::PSK16: return LIQUID_MODEM_PSK16;
        case ModulationScheme::PSK32: return LIQUID_MODEM_PSK32;
        case ModulationScheme::PSK64: return LIQUID_MODEM_PSK64;
        case ModulationScheme::QAM4: return LIQUID_MODEM_QAM4;
        case ModulationScheme::QAM16: return LIQUID_MODEM_QAM16;
        case ModulationScheme::QAM32: return LIQUID_MODEM_QAM32;
        case ModulationScheme::QAM64: return LIQUID_MODEM_QAM64;
        case ModulationScheme::QAM128: return LIQUID_MODEM_QAM128;
        case ModulationScheme::QAM256: return LIQUID_MODEM_QAM256;
        case ModulationScheme::ASK2: return LIQUID_MODEM_ASK2;
        case ModulationScheme::ASK4: return LIQUID_MODEM_ASK4;
        case ModulationScheme::ASK8: return LIQUID_MODEM_ASK8;
        case ModulationScheme::ASK16: return LIQUID_MODEM_ASK16;
        case ModulationScheme::OOK: return LIQUID_MODEM_OOK;
    }
    throw std::runtime_error("recover_linear: unknown modulation scheme");
}

// Coarse carrier frequency offset, via the M-th power method: raising the signal to
// the M-th power collapses an M-fold symmetric constellation's modulation down to a
// single spectral line at M times the true offset, found here as the FFT's peak
// magnitude bin. M=2 for binary (BPSK/ASK2/OOK) constellations, which really are
// 2-fold symmetric; M=4 otherwise -- an approximation for anything above QPSK (true
// 8-PSK symmetry would want M=8, and so on), traded for one CFO estimator that works
// across every scheme this project supports, since the fine carrier tracking loop
// below (decision-directed, scheme-aware via the real modem) only needs this coarse
// estimate to land within its own capture range, not to be exact.
float estimate_coarse_cfo(const std::vector<std::complex<float>>& x, unsigned int m_power) {
    std::vector<std::complex<float>> powered(x.size());
    for (size_t i = 0; i < x.size(); ++i) {
        std::complex<float> p = x[i];
        for (unsigned int k = 1; k < m_power; ++k) p *= x[i];
        powered[i] = p;
    }
    const auto spectrum = fft_forward(powered);
    const size_t n = spectrum.size();

    size_t peak_bin = 0;
    float peak_mag = -1.0f;
    for (size_t i = 0; i < n; ++i) {
        const float mag = std::norm(spectrum[i]);
        if (mag > peak_mag) {
            peak_mag = mag;
            peak_bin = i;
        }
    }
    const long signed_bin =
        (peak_bin <= n / 2) ? static_cast<long>(peak_bin) : static_cast<long>(peak_bin) - static_cast<long>(n);
    return (static_cast<float>(signed_bin) / static_cast<float>(n)) / static_cast<float>(m_power);
}

std::vector<std::complex<float>> mix_by(const std::vector<std::complex<float>>& x, float cycles_per_sample) {
    std::vector<std::complex<float>> out(x.size());
    for (size_t i = 0; i < x.size(); ++i) {
        const float phase = -2.0f * static_cast<float>(M_PI) * cycles_per_sample * static_cast<float>(i);
        out[i] = x[i] * std::complex<float>(std::cos(phase), std::sin(phase));
    }
    return out;
}

}  // namespace

RecoveryResult recover_linear(const std::vector<std::complex<float>>& raw, unsigned int sps,
                              ModulationScheme scheme) {
    if (raw.empty()) {
        throw std::runtime_error("recover_linear: samples is empty");
    }
    if (sps < 2) {
        throw std::runtime_error("recover_linear: sps must be >= 2 (nothing to recover at 1 sample/symbol)");
    }

    modemcf mod = modemcf_create(to_liquid_scheme(scheme));
    const unsigned int bps = modemcf_get_bps(mod);
    const unsigned int m_power = (bps <= 1) ? 2u : 4u;  // see estimate_coarse_cfo's comment

    // 1. Coarse CFO: get the loop below within its capture range.
    const float coarse_cfo = estimate_coarse_cfo(raw, m_power);
    const auto mixed = mix_by(raw, coarse_cfo);

    // 2. Timing recovery: an RRC-matched polyphase symbol synchronizer, which both
    // filters and resamples to (as close as it can lock to) exactly one output
    // sample per input symbol, tracking whatever the actual (possibly fractional,
    // possibly drifting) timing offset turns out to be -- unlike naive decimation,
    // which only ever samples at a fixed, assumed-zero phase.
    const unsigned int delay_symbols = 3;
    const float rolloff = 0.35f;
    const unsigned int num_filters = 32;
    symsync_crcf sync = symsync_crcf_create_rnyquist(LIQUID_FIRFILT_RRC, sps, delay_symbols, rolloff, num_filters);
    symsync_crcf_set_lf_bw(sync, 0.01f);

    std::vector<std::complex<float>> timed(mixed.size());
    unsigned int n_written = 0;
    symsync_crcf_execute(sync, const_cast<liquid_float_complex*>(as_liquid(const_cast<std::complex<float>*>(mixed.data()))),
                         static_cast<unsigned int>(mixed.size()), as_liquid(timed.data()), &n_written);
    timed.resize(n_written);
    symsync_crcf_destroy(sync);

    // 3. Fine carrier/phase tracking: a decision-directed Costas loop, using the same
    // modem object's own phase-error estimate (liquid's standard technique) to steer
    // an NCO, symbol by symbol, correcting whatever coarse step 1 left behind plus any
    // slow phase drift across the capture.
    nco_crcf carrier = nco_crcf_create(LIQUID_VCO);
    nco_crcf_pll_set_bandwidth(carrier, 0.02f);

    std::vector<std::complex<float>> tracked(timed.size());
    for (size_t i = 0; i < timed.size(); ++i) {
        std::complex<float> corrected;
        nco_crcf_mix_down(carrier, *as_liquid(&timed[i]), as_liquid(&corrected));
        unsigned int symbol = 0;
        modemcf_demodulate(mod, *as_liquid(&corrected), &symbol);
        const float phase_error = modemcf_get_demodulator_phase_error(mod);
        nco_crcf_pll_step(carrier, phase_error);
        nco_crcf_step(carrier);
        tracked[i] = corrected;
    }
    nco_crcf_destroy(carrier);
    modemcf_destroy(mod);

    RecoveryResult result;
    result.samples = std::move(tracked);
    result.coarse_cfo_cycles_per_sample = coarse_cfo;
    result.phase_ambiguity_count = m_power;
    return result;
}

}  // namespace sigcore
