#include "recover.hpp"

#include <algorithm>
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

// Timing recovery for a Rect (unshaped NRZ) pulse: integrate-and-dump at each of the
// `sps` possible sample phases, and keep the one whose average symbol magnitude is
// largest. Mean magnitude, not mean value, so this does not need the carrier phase
// already resolved (a rotating, not-yet-phase-locked constellation still averages to
// a large magnitude at the correctly-aligned phase, and toward zero at a misaligned
// one, since a misaligned integration window straddles two different symbols'
// worth of signal and partially cancels) -- the standard "maximum eye opening"
// technique for a pulse with no matched filter to lock onto instead.
std::vector<std::complex<float>> integrate_and_dump(const std::vector<std::complex<float>>& x, unsigned int sps) {
    unsigned int best_phase = 0;
    double best_score = -1.0;
    for (unsigned int phase = 0; phase < sps; ++phase) {
        const size_t n = (x.size() > phase) ? (x.size() - phase) / sps : 0;
        if (n == 0) continue;
        double sum_mag = 0.0;
        for (size_t k = 0; k < n; ++k) {
            std::complex<float> acc(0.0f, 0.0f);
            for (unsigned int j = 0; j < sps; ++j) acc += x[phase + k * sps + j];
            sum_mag += std::abs(acc);
        }
        const double score = sum_mag / static_cast<double>(n);
        if (score > best_score) {
            best_score = score;
            best_phase = phase;
        }
    }

    const size_t n = (x.size() > best_phase) ? (x.size() - best_phase) / sps : 0;
    std::vector<std::complex<float>> out(n);
    for (size_t k = 0; k < n; ++k) {
        std::complex<float> acc(0.0f, 0.0f);
        for (unsigned int j = 0; j < sps; ++j) acc += x[best_phase + k * sps + j];
        out[k] = acc / static_cast<float>(sps);
    }
    return out;
}

// Measured: 0.0005-0.002 holds lock on 8-PSK at 20-30 dB; 0.003 and up slip cycles mid-stream.
constexpr size_t kPrerollSymbols = 256;
constexpr size_t kQamPhaseWindow = 64;

bool is_square_qam(ModulationScheme s) {
    return s == ModulationScheme::QAM16 || s == ModulationScheme::QAM64 || s == ModulationScheme::QAM256;
}
constexpr float kPsk8LoopBandwidth = 0.001f;

}  // namespace

RecoveryResult recover_linear(const std::vector<std::complex<float>>& raw, unsigned int sps,
                              ModulationScheme scheme, PulseShape pulse, float rolloff_factor) {
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
    // 8-PSK needs the 8th power: its 4th power collapses to a +/-1 (BPSK-like) signal with no carrier line.
    const float coarse_cfo = estimate_coarse_cfo(raw, scheme == ModulationScheme::PSK8 ? 8u : m_power);
    const auto mixed = mix_by(raw, coarse_cfo);

    // 2. Timing recovery. Rrc: an RRC-matched polyphase symbol synchronizer, which
    // both filters and resamples to (as close as it can lock to) exactly one output
    // sample per input symbol, tracking whatever the actual (possibly fractional,
    // possibly drifting) timing offset turns out to be -- unlike naive decimation,
    // which only ever samples at a fixed, assumed-zero phase. Rect: see
    // integrate_and_dump's own comment -- a different technique entirely, for a
    // pulse shape Rrc is the wrong matched filter for (see PulseShape's comment).
    std::vector<std::complex<float>> timed;
    if (pulse == PulseShape::Rrc) {
        const unsigned int delay_symbols = 3;
        const float rolloff = rolloff_factor;
        const unsigned int num_filters = 32;
        symsync_crcf sync = symsync_crcf_create_rnyquist(LIQUID_FIRFILT_RRC, sps, delay_symbols, rolloff, num_filters);
        symsync_crcf_set_lf_bw(sync, 0.01f);

        timed.resize(mixed.size());
        unsigned int n_written = 0;
        // The timing loop needs a few hundred symbols to settle, which corrupts the start of a short
        // capture; the dense constellations cannot tolerate that. Run it over the first stretch once
        // (output discarded), then feed the capture from the start with the settled loop.
        if (scheme == ModulationScheme::PSK8 || is_square_qam(scheme)) {
            const size_t pre = std::min<size_t>(mixed.size(), static_cast<size_t>(sps) * kPrerollSymbols);
            std::vector<std::complex<float>> scratch(pre + 16);
            unsigned int scratch_written = 0;
            symsync_crcf_execute(sync, const_cast<liquid_float_complex*>(as_liquid(const_cast<std::complex<float>*>(mixed.data()))),
                                 static_cast<unsigned int>(pre), as_liquid(scratch.data()), &scratch_written);
        }
        symsync_crcf_execute(sync, const_cast<liquid_float_complex*>(as_liquid(const_cast<std::complex<float>*>(mixed.data()))),
                             static_cast<unsigned int>(mixed.size()), as_liquid(timed.data()), &n_written);
        timed.resize(n_written);
        symsync_crcf_destroy(sync);
    } else {
        timed = integrate_and_dump(mixed, sps);
    }

    // 2b. Amplitude. The matched filter's gain is not exactly unity, and QAM decides on amplitude as
    // well as phase, so scale to the unit average power every constellation here is defined with.
    // (Constant-modulus schemes are unaffected by a gain error, so they are left alone.)
    if (scheme == ModulationScheme::QAM16 || scheme == ModulationScheme::QAM32 || scheme == ModulationScheme::QAM64 ||
        scheme == ModulationScheme::QAM128 || scheme == ModulationScheme::QAM256) {
        double power = 0.0;
        for (const auto& v : timed) power += std::norm(v);
        power /= static_cast<double>(std::max<size_t>(1, timed.size()));
        if (power > 0.0) {
            const float gain = static_cast<float>(1.0 / std::sqrt(power));
            for (auto& v : timed) v *= gain;
        }
    }

    // 3. Fine carrier/phase tracking: a decision-directed Costas loop, using the same
    // modem object's own phase-error estimate (liquid's standard technique) to steer
    // an NCO, symbol by symbol, correcting whatever coarse step 1 left behind plus any
    // slow phase drift across the capture.
    nco_crcf carrier = nco_crcf_create(LIQUID_VCO);
    // 8-PSK's decision regions are narrow: the 0.02 used for the other schemes is too noisy and slips.
    nco_crcf_pll_set_bandwidth(carrier, scheme == ModulationScheme::PSK8 ? kPsk8LoopBandwidth : 0.02f);

    std::vector<std::complex<float>> tracked(timed.size());
    // The decision-directed loop needs a few hundred symbols to pull in, which leaves the start of a
    // short capture wrong and fails FEC verification. For 8-PSK, run
    // it over the first symbols once to lock (8-PSK; QAM uses the feed-forward estimator below), then restart from symbol 0 keeping the locked state.
    const bool preroll = scheme == ModulationScheme::PSK8;
    if (preroll) {
        const size_t n = std::min<size_t>(timed.size(), kPrerollSymbols);
        for (size_t i = 0; i < n; ++i) {
            std::complex<float> corrected;
            nco_crcf_mix_down(carrier, *as_liquid(&timed[i]), as_liquid(&corrected));
            unsigned int symbol = 0;
            modemcf_demodulate(mod, *as_liquid(&corrected), &symbol);
            nco_crcf_pll_step(carrier, modemcf_get_demodulator_phase_error(mod));
            nco_crcf_step(carrier);
        }
    }
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

    if (is_square_qam(scheme)) {
        // Feed-forward carrier phase for QAM: a decision-directed loop is unreliable on dense
        // constellations (wrong decisions steer it), so estimate the phase from s^4 instead. Square
        // QAM has E[s^4] real and negative (cumulant -0.68 for QAM16, -0.62 for QAM64), so
        // arg(sum s^4) = pi + 4*theta over a sliding window; the 4-fold ambiguity is left to the
        // caller's rotation search, exactly as with the loop.
        const size_t n = timed.size();
        const long half = static_cast<long>(kQamPhaseWindow / 2);
        std::vector<std::complex<double>> prefix(n + 1);
        for (size_t i = 0; i < n; ++i) {
            const std::complex<double> v(timed[i].real(), timed[i].imag());
            const std::complex<double> v2 = v * v;
            prefix[i + 1] = prefix[i] + v2 * v2;
        }
        double prev = 0.0;
        bool first = true;
        for (size_t i = 0; i < n; ++i) {
            const size_t lo = static_cast<size_t>(std::max<long>(0, static_cast<long>(i) - half));
            const size_t hi = static_cast<size_t>(std::min<long>(static_cast<long>(n), static_cast<long>(i) + half + 1));
            double phi = std::arg(prefix[hi] - prefix[lo]) - M_PI;  // = 4 * theta (mod 2 pi)
            if (!first) {
                while (phi - prev > M_PI) phi -= 2.0 * M_PI;
                while (phi - prev < -M_PI) phi += 2.0 * M_PI;
            }
            first = false;
            prev = phi;
            const double theta = phi / 4.0;
            tracked[i] = timed[i] * std::complex<float>(static_cast<float>(std::cos(-theta)), static_cast<float>(std::sin(-theta)));
        }
    }

    RecoveryResult result;
    result.samples = std::move(tracked);
    result.coarse_cfo_cycles_per_sample = coarse_cfo;
    // The CFO estimator above uses M=4 for anything past BPSK, but the carrier loop's own lock
    // ambiguity is the constellation's true rotational symmetry: 8 for 8-PSK.
    result.phase_ambiguity_count = (scheme == ModulationScheme::PSK8) ? 8u : m_power;
    return result;
}

}  // namespace sigcore
