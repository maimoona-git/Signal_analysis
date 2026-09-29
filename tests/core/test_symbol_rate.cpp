// Blind symbol-rate (samples-per-symbol) estimation (see core/include/symbol_rate.hpp).
// Built and checked against synthetic captures across both pulse shapes this project
// supports (see recover.hpp's PulseShape) and both a constant-modulus scheme (BPSK,
// where a plain envelope has no periodicity to find at all -- see symbol_rate.hpp's own
// comment on why the estimator does NOT just use |x[n]|^2) and a non-constant-modulus
// one (QAM16), at several true samples-per-symbol values, with noise -- not a single
// hand-picked easy case.

#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

#include <liquid/liquid.h>

#include "demod.hpp"
#include "symbol_rate.hpp"
#include "../harness.hpp"
#include "../testutil.hpp"

using namespace sigcore;
using namespace sigtest;
using cf = std::complex<float>;

namespace {

liquid_float_complex* as_liquid(cf* p) { return reinterpret_cast<liquid_float_complex*>(p); }

void add_noise(std::vector<cf>& x, float amp, uint64_t seed) {
    uint64_t rng = seed;
    auto next_noise = [&]() {
        rng = rng * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<float>((rng >> 40) & 0xFFFFFF) / static_cast<float>(0xFFFFFF) - 0.5f;
    };
    for (auto& s : x) s += cf(next_noise(), next_noise()) * amp;
}

// Zero-order-hold (rectangular, unshaped) pulses at `sps` samples/symbol.
std::vector<cf> hold_pulses(const std::vector<cf>& symbols, unsigned int sps) {
    std::vector<cf> out(symbols.size() * sps);
    for (size_t i = 0; i < symbols.size(); ++i)
        for (unsigned int k = 0; k < sps; ++k) out[i * sps + k] = symbols[i];
    return out;
}

// Real RRC pulse shaping via liquid's own TX-side interpolator (same technique as
// test_recover.cpp; see its own comment for why this, not a cruder stand-in).
std::vector<cf> rrc_pulses(const std::vector<cf>& symbols, unsigned int sps) {
    const unsigned int delay_symbols = 3;
    const float rolloff = 0.35f;
    firinterp_crcf tx = firinterp_crcf_create_prototype(LIQUID_FIRFILT_RRC, sps, delay_symbols, rolloff, 0.0f);
    std::vector<cf> shaped(symbols.size() * sps);
    for (size_t i = 0; i < symbols.size(); ++i) {
        cf sym = symbols[i];
        firinterp_crcf_execute(tx, *as_liquid(&sym), as_liquid(&shaped[i * sps]));
    }
    firinterp_crcf_destroy(tx);
    return shaped;
}

}  // namespace

TEST_CASE("estimate_symbol_rate: finds the true sps across pulse shapes, schemes and rates") {
    struct Case {
        const char* name;
        ModulationScheme scheme;
        unsigned int sps;
        bool rrc;
    };
    const Case cases[] = {
        {"BPSK, Rect, sps=8", ModulationScheme::BPSK, 8, false},
        {"BPSK, Rect, sps=16", ModulationScheme::BPSK, 16, false},
        {"BPSK, Rrc, sps=8", ModulationScheme::BPSK, 8, true},
        {"QAM16, Rect, sps=8", ModulationScheme::QAM16, 8, false},
        {"QAM16, Rrc, sps=12", ModulationScheme::QAM16, 12, true},
        {"BPSK, Rrc, sps=4", ModulationScheme::BPSK, 4, true},
    };

    for (const auto& c : cases) {
        const auto bits = random_bits(4000, /*seed=*/static_cast<uint64_t>(c.sps) * 7 + (c.rrc ? 1 : 0));
        const auto symbols = linear_modulate(bits, c.scheme);
        auto samples = c.rrc ? rrc_pulses(symbols, c.sps) : hold_pulses(symbols, c.sps);
        add_noise(samples, 0.03f, 999);

        const auto est = estimate_symbol_rate(samples, /*min_sps=*/2.0, /*max_sps=*/64.0);
        const double rel_err = std::abs(est.samples_per_symbol - static_cast<double>(c.sps)) / c.sps;

        // Within 5% of the true rate, at a search range spanning 2..64: a coarse
        // estimate good enough to seed the rest of the pipeline (samples_per_symbol
        // is otherwise a required, exact GUI/API input -- see recover.hpp's own
        // comment on why blind rate estimation was explicitly out of scope there).
        CHECK(rel_err < 0.05);
        CHECK(est.confidence > 0.2);  // a real, found line, not noise-floor guessing
    }
}

TEST_CASE("estimate_symbol_rate: a narrower search range still finds the same true rate") {
    const auto bits = random_bits(4000, 55);
    const auto symbols = linear_modulate(bits, ModulationScheme::QPSK);
    auto samples = rrc_pulses(symbols, 10);
    add_noise(samples, 0.03f, 1234);

    const auto wide = estimate_symbol_rate(samples, 2.0, 64.0);
    const auto narrow = estimate_symbol_rate(samples, 6.0, 20.0);
    CHECK(std::abs(wide.samples_per_symbol - narrow.samples_per_symbol) < 0.5);
    CHECK(std::abs(narrow.samples_per_symbol - 10.0) / 10.0 < 0.05);
}

TEST_CASE("estimate_symbol_rate: confidence reads ~0 on pure noise, not a misleadingly high ratio") {
    // An early version of the confidence metric compared the found peak only
    // against the mean of the rest of the search range, and measured a
    // consistent, misleadingly high ~0.7-0.8 on pure noise (see
    // symbol_rate.cpp's own comment on why: the expected maximum of many
    // independent noise bins is itself well above their mean, an extreme-
    // value-statistics effect, not a detection). Confirmed here across
    // several lengths and seeds, not just one lucky draw.
    for (size_t n : {8000u, 32000u, 131072u}) {
        for (uint64_t seed : {42ull, 43ull, 99ull, 12345ull}) {
            std::vector<cf> noise_only(n);
            add_noise(noise_only, 1.0f, seed);
            const auto est = estimate_symbol_rate(noise_only, 2.0, 64.0);
            CHECK(est.confidence < 0.1);
        }
    }
}

TEST_CASE("estimate_symbol_rate: confidence degrades to ~0 exactly where the estimate itself stops being trustworthy") {
    // A real fail-safe property, not just "confidence looks reasonable":
    // below some noise level the true line is no longer the tallest peak in
    // the search range at all, and the estimate itself goes wrong (not just
    // noisy) -- confidence should collapse to ~0 at exactly that point, not
    // stay confidently wrong.
    const auto bits = random_bits(4000, 88);
    const auto symbols = linear_modulate(bits, ModulationScheme::BPSK);
    const auto clean = rrc_pulses(symbols, 8);

    auto good = clean;
    add_noise(good, 0.5f, 555);
    const auto est_good = estimate_symbol_rate(good, 2.0, 64.0);
    CHECK(est_good.confidence > 0.8);
    CHECK(std::abs(est_good.samples_per_symbol - 8.0) / 8.0 < 0.05);

    auto bad = clean;
    add_noise(bad, 4.0f, 555);  // far past where the true line is still the tallest peak
    const auto est_bad = estimate_symbol_rate(bad, 2.0, 64.0);
    CHECK(est_bad.confidence < 0.1);
}

TEST_CASE("estimate_symbol_rate rejects degenerate input") {
    bool threw = false;
    try {
        estimate_symbol_rate(std::vector<cf>(10, cf(1, 0)));  // too short
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);

    threw = false;
    try {
        estimate_symbol_rate(std::vector<cf>(1000, cf(1, 0)), /*min_sps=*/1.5, /*max_sps=*/64.0);  // min_sps < 2
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);

    threw = false;
    try {
        estimate_symbol_rate(std::vector<cf>(1000, cf(1, 0)), /*min_sps=*/32.0, /*max_sps=*/8.0);  // max <= min
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

TEST_MAIN()
