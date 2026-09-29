// Real-world signal support: carrier and timing recovery for linear modulations
// (see core/include/recover.hpp). Built and checked against a synthetic capture
// deliberately made to resemble a real, un-synchronized RF recording -- pulse-shaped
// (not this project's own zero-order-hold synthetic convention), a fractional/unknown
// start phase, a carrier frequency offset, and light noise -- not the exact, already-
// aligned captures the rest of the pipeline's tests use, so this measures the actual
// gap naive decimation has on a realistic signal, not a strawman.

#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

#include <liquid/liquid.h>

#include "demod.hpp"
#include "recover.hpp"
#include "../harness.hpp"
#include "../testutil.hpp"

using namespace sigcore;
using namespace sigtest;
using cf = std::complex<float>;

namespace {

liquid_float_complex* as_liquid(cf* p) { return reinterpret_cast<liquid_float_complex*>(p); }

// Real, matched RRC pulse shaping (liquid's own TX-side interpolator, the exact
// counterpart of recover.cpp's RX-side matched filter -- same filter family, delay
// and rolloff, the way an actual transmitter/receiver pair would agree on a pulse
// shape), a carrier offset, noise, and a non-symbol-aligned start -- everything
// naive `[0, sps, 2*sps, ...]` decimation assumes is absent, and everything a real
// capture actually has. A cruder stand-in (e.g. a rectangular hold, lightly
// lowpassed) was tried first and turned out to prove nothing either way: sampling a
// rectangular hold at *any* fixed offset happens to land on the same value, so
// naive decimation "worked" on it regardless of the impairment, and a mismatched,
// non-Nyquist TX/RX filter pairing corrupts even perfectly-timed samples with
// inter-symbol interference the receiver has no way to remove -- neither is what a
// real matched-filter link actually looks like.
std::vector<cf> make_impaired_capture(const std::vector<cf>& symbols, unsigned int sps, float cfo,
                                      float noise_amp, unsigned int start_offset, uint64_t noise_seed) {
    const unsigned int delay_symbols = 3;
    const float rolloff = 0.35f;
    firinterp_crcf tx = firinterp_crcf_create_prototype(LIQUID_FIRFILT_RRC, sps, delay_symbols, rolloff, 0.0f);
    std::vector<cf> shaped(symbols.size() * sps);
    for (size_t i = 0; i < symbols.size(); ++i) {
        cf sym = symbols[i];
        firinterp_crcf_execute(tx, *as_liquid(&sym), as_liquid(&shaped[i * sps]));
    }
    firinterp_crcf_destroy(tx);

    std::vector<cf> impaired;
    impaired.reserve(shaped.size() - start_offset);
    uint64_t rng = noise_seed;
    auto next_noise = [&]() {
        rng = rng * 6364136223846793005ull + 1442695040888963407ull;
        const float u = static_cast<float>((rng >> 40) & 0xFFFFFF) / static_cast<float>(0xFFFFFF) - 0.5f;
        return u;
    };
    for (size_t i = start_offset; i < shaped.size(); ++i) {
        const float phase = -2.0f * static_cast<float>(M_PI) * cfo * static_cast<float>(i - start_offset);
        cf sample = shaped[i] * cf(std::cos(phase), std::sin(phase));
        sample += cf(next_noise(), next_noise()) * noise_amp;
        impaired.push_back(sample);
    }
    return impaired;
}

// Zero-order-hold (rectangular) pulses -- no transmit pulse shaping at all -- plus a
// carrier offset and light noise: the exact shape of PySDR's own real
// bpsk_rect_20sps.sigmf-data test file (see the session notes this was found against),
// which is what motivated PulseShape existing at all -- Rrc mode (the only mode before
// this) measurably made this kind of signal *worse* than doing nothing, because an RRC
// filter is not a matched filter for a pulse that was never RRC-shaped to begin with.
std::vector<cf> make_rect_impaired_capture(const std::vector<cf>& symbols, unsigned int sps, float cfo,
                                           float noise_amp, unsigned int start_offset, uint64_t noise_seed) {
    std::vector<cf> held(symbols.size() * sps);
    for (size_t i = 0; i < symbols.size(); ++i)
        for (unsigned int k = 0; k < sps; ++k) held[i * sps + k] = symbols[i];

    std::vector<cf> impaired;
    impaired.reserve(held.size() - start_offset);
    uint64_t rng = noise_seed;
    auto next_noise = [&]() {
        rng = rng * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<float>((rng >> 40) & 0xFFFFFF) / static_cast<float>(0xFFFFFF) - 0.5f;
    };
    for (size_t i = start_offset; i < held.size(); ++i) {
        const float phase = -2.0f * static_cast<float>(M_PI) * cfo * static_cast<float>(i - start_offset);
        cf sample = held[i] * cf(std::cos(phase), std::sin(phase));
        sample += cf(next_noise(), next_noise()) * noise_amp;
        impaired.push_back(sample);
    }
    return impaired;
}

// The same alignment-search scoring test_recover.cpp's other test uses (see its own
// comment): a real lock's start point is not precomputable, so this searches for the
// best-matching window rather than assuming index 0, for both phase hypotheses --
// and in both directions, since a real capture can just as easily start with extra
// leading samples ahead of the message (recovered_bits[off+i] vs want[i]) as it can
// start partway into the message, having lost its first few bits to trimming or a
// timing-recovery lock point that lands on the first *complete* symbol rather than
// the first symbol overall (want[off+i] vs recovered_bits[i]) -- searching only one
// direction silently misses the other and reports a false failure.
unsigned int best_bit_errors(const std::vector<uint8_t>& recovered_bits, const std::vector<uint8_t>& want, size_t window) {
    unsigned int best = static_cast<unsigned int>(window);
    for (size_t off = 0; off + window <= recovered_bits.size() && window <= want.size(); ++off) {
        unsigned int mismatches = 0, inverted = 0;
        for (size_t i = 0; i < window; ++i) {
            const bool a = recovered_bits[off + i] != 0;
            const bool b = want[i] != 0;
            mismatches += (a != b);
            inverted += ((!a) != b);
        }
        best = std::min({best, mismatches, inverted});
        if (best == 0) return best;
    }
    for (size_t off = 1; off + window <= want.size() && window <= recovered_bits.size(); ++off) {
        unsigned int mismatches = 0, inverted = 0;
        for (size_t i = 0; i < window; ++i) {
            const bool a = recovered_bits[i] != 0;
            const bool b = want[off + i] != 0;
            mismatches += (a != b);
            inverted += ((!a) != b);
        }
        best = std::min({best, mismatches, inverted});
        if (best == 0) break;
    }
    return best;
}

}  // namespace

TEST_CASE("recover_linear: PulseShape::Rect beats Rrc on an unshaped (rectangular) real capture") {
    const auto bits = random_bits(1600, /*seed=*/11);
    const auto symbols = linear_modulate(bits, ModulationScheme::BPSK);
    // sps=8, not PySDR's literal 20 (see make_rect_impaired_capture's own comment for
    // what motivated this test): at 20 samples/symbol there's enough oversampling
    // margin that a mismatched RRC filter's ISI doesn't move any decision past the
    // slicer boundary either -- the gap is real but sps-dependent, and 8 is where it
    // is reliably measurable without relying on a lucky noise draw.
    const unsigned int sps = 8;
    const auto impaired = make_rect_impaired_capture(symbols, sps, /*cfo=*/0.003f, /*noise_amp=*/0.02f,
                                                      /*start_offset=*/7, /*noise_seed=*/777);

    const auto rrc_result = recover_linear(impaired, sps, ModulationScheme::BPSK, PulseShape::Rrc);
    const auto rrc_bits = linear_demodulate(rrc_result.samples, ModulationScheme::BPSK);
    const auto rrc_errors = best_bit_errors(rrc_bits, bits, 400);

    const auto rect_result = recover_linear(impaired, sps, ModulationScheme::BPSK, PulseShape::Rect);
    const auto rect_bits = linear_demodulate(rect_result.samples, ModulationScheme::BPSK);
    const auto rect_errors = best_bit_errors(rect_bits, bits, 400);

    // The actual claim: the right pulse-shape mode gives a clean decode where the
    // wrong one measurably does not -- not just "is better", but "actually works".
    CHECK(rect_errors < 400u / 20);  // under 5% wrong at the best alignment: a real decode
    CHECK(rect_errors < rrc_errors);  // and clearly better than using the wrong filter
}

TEST_CASE("recover_linear fixes a pulse-shaped, offset, CFO-impaired BPSK capture naive decimation gets wrong") {
    const auto bits = random_bits(1600, /*seed=*/7);
    const auto symbols = linear_modulate(bits, ModulationScheme::BPSK);
    const unsigned int sps = 8;
    const auto impaired = make_impaired_capture(symbols, sps, /*cfo=*/0.0037f, /*noise_amp=*/0.03f,
                                                /*start_offset=*/3, /*noise_seed=*/12345);

    // Naive decimation (what the pipeline falls back to today): sanity-check this
    // really is a hard case, not a strawman -- it should come out mostly wrong.
    std::vector<cf> naive;
    for (size_t i = 0; i < impaired.size(); i += sps) naive.push_back(impaired[i]);
    const auto naive_bits = linear_demodulate(naive, ModulationScheme::BPSK);
    unsigned int naive_wrong = 0;
    for (size_t i = 0; i < std::min(naive_bits.size(), bits.size()); ++i) naive_wrong += (naive_bits[i] != bits[i]);
    CHECK(naive_wrong > bits.size() / 6);  // far from a clean decode

    // recover_linear should do far better: run its output through the same
    // linear_demodulate the pipeline already uses, and check the trailing portion
    // (after the matched filter's group delay and the carrier loop's lock-in
    // transient, both expected and both documented in recover.hpp) matches the
    // original message -- up to BPSK's own 180-degree phase ambiguity (see
    // RecoveryResult's own comment): a constant inversion of every bit is an
    // equally valid lock, not an error.
    const auto result = recover_linear(impaired, sps, ModulationScheme::BPSK);
    CHECK(result.phase_ambiguity_count == 2);
    const auto recovered_bits = linear_demodulate(result.samples, ModulationScheme::BPSK);

    CHECK(recovered_bits.size() > 400);
    // A real timing/carrier recovery loop's lock-in point does not land at a
    // precomputable symbol index (it depends on exactly where the loop's filters and
    // PLL happen to settle, not just their nominal group delay) -- the same reason
    // the real pipeline has its own bitstream-correlation stage downstream, to find
    // where a decoded message actually starts rather than assume index 0. Mirror that
    // here: search a range of candidate start offsets (and, for BPSK, both phase
    // hypotheses) and require a clean decode at *some* alignment, not at one guessed
    // in advance.
    const size_t window = 400;  // long enough that a chance alignment cannot fake a clean match
    unsigned int best_errors = window;
    for (size_t off = 0; off + window <= recovered_bits.size() && off + window <= bits.size(); off += 1) {
        unsigned int mismatches = 0, inverted = 0;
        for (size_t i = 0; i < window; ++i) {
            const bool a = recovered_bits[off + i] != 0;
            const bool b = bits[i] != 0;  // compare against the message's own start
            mismatches += (a != b);
            inverted += ((!a) != b);
        }
        best_errors = std::min({best_errors, mismatches, inverted});
        if (best_errors == 0) break;
    }
    CHECK(best_errors < window / 20);  // under 5% wrong, at the best alignment -- a real decode, not noise
}

TEST_CASE("recover_linear rejects degenerate input") {
    bool threw = false;
    try {
        recover_linear({}, 8, ModulationScheme::BPSK);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);

    threw = false;
    try {
        recover_linear({cf(1, 0), cf(-1, 0)}, 1, ModulationScheme::BPSK);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

TEST_MAIN()
