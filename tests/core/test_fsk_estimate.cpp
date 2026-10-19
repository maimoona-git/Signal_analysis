// Blind M-FSK parameter estimation (see core/include/fsk_estimate.hpp), against captures
// built by the same fsk_modulate the demodulator inverts, across tone counts, symbol
// lengths and bandwidths, with noise.

#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

#include "demod.hpp"
#include "fsk_estimate.hpp"
#include "../harness.hpp"
#include "../testutil.hpp"

using namespace sigcore;

namespace {
std::vector<std::complex<float>> make(unsigned bps, unsigned sps, float bw, float noise, unsigned seed) {
    const auto bits = sigtest::random_bits(bps * 400, seed);
    auto x = fsk_modulate(std::vector<uint8_t>(bits.begin(), bits.end()), bps, sps, bw);
    uint32_t s = seed * 2654435761u + 1;
    auto rnd = [&] { s = s * 1664525u + 1013904223u; return static_cast<float>(s >> 8) / 8388608.0f - 1.0f; };
    for (auto& v : x) v += std::complex<float>(noise * rnd(), noise * rnd());
    return x;
}
}  // namespace

TEST_CASE("estimate_fsk_params recovers tone count, symbol length and bandwidth") {
    struct C { unsigned bps, sps; float bw; };
    for (const C c : {C{1, 8, 0.25f}, C{1, 16, 0.1f}, C{2, 16, 0.25f}, C{2, 12, 0.15f}, C{3, 32, 0.25f}}) {
        const auto e = estimate_fsk_params(make(c.bps, c.sps, c.bw, 0.02f, 7 + c.bps));
        CHECK(e.bits_per_symbol == c.bps);
        CHECK(e.samples_per_symbol == c.sps);
        CHECK_NEAR(e.bandwidth, c.bw, 0.02f);
        CHECK(e.confidence > 0.8);
    }
}

TEST_CASE("estimate_fsk_params has low confidence on non-FSK input and rejects short input") {
    const auto bits = sigtest::random_bits(800, 3);
    std::vector<std::complex<float>> noise(4096);
    uint32_t s = 99;
    for (auto& v : noise) {
        s = s * 1664525u + 1013904223u; const float a = static_cast<float>(s >> 8) / 8388608.0f - 1.0f;
        s = s * 1664525u + 1013904223u; const float b = static_cast<float>(s >> 8) / 8388608.0f - 1.0f;
        v = {a, b};
    }
    CHECK(estimate_fsk_params(noise).confidence < 0.8);
    CHECK_THROWS(estimate_fsk_params(std::vector<std::complex<float>>(10)));
}

TEST_MAIN()
