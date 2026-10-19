// The blind "is this really FSK?" check (core/include/modulation_blind.hpp): it must catch linear
// modulations the classifier mislabels, and -- more important -- never turn real FSK into BPSK.

#include <cmath>
#include <complex>
#include <cstdint>
#include <random>
#include <vector>

#include <liquid/liquid.h>

#include "demod.hpp"
#include "modulation_blind.hpp"
#include "../harness.hpp"
#include "../testutil.hpp"

using namespace sigcore;
using cf = std::complex<float>;

namespace {

std::vector<cf> shaped(const std::vector<cf>& symbols, unsigned sps, float rolloff) {
    firinterp_crcf tx = firinterp_crcf_create_prototype(LIQUID_FIRFILT_RRC, sps, 3, rolloff, 0.0f);
    std::vector<cf> y(symbols.size() * sps);
    for (size_t i = 0; i < symbols.size(); ++i) {
        cf s = symbols[i];
        firinterp_crcf_execute(tx, *reinterpret_cast<liquid_float_complex*>(&s), reinterpret_cast<liquid_float_complex*>(&y[i * sps]));
    }
    firinterp_crcf_destroy(tx);
    return y;
}

void impair(std::vector<cf>& x, double cfo, double snr_db, std::mt19937& rng) {
    double power = 0;
    for (auto& v : x) power += std::norm(v);
    power /= static_cast<double>(x.size());
    const double sigma = std::sqrt(power / std::pow(10.0, snr_db / 10.0) / 2.0);
    std::normal_distribution<float> g;
    for (size_t n = 0; n < x.size(); ++n) {
        const double ph = 2.0 * M_PI * cfo * static_cast<double>(n);
        x[n] = x[n] * cf(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph))) +
               cf(static_cast<float>(sigma) * g(rng), static_cast<float>(sigma) * g(rng));
    }
}

// A real capture is a burst with idle noise before and after it (the beacon recordings are exactly
// that, with the burst a third to a half of the file), so the test signals are too: the burst's own
// length of silence on each side, to which impair() then adds noise. (With only a quarter of
// padding the envelope variation measured 0.21-0.30, under the check's 0.3 -- it needs a real burst.)
std::vector<cf> as_burst(const std::vector<cf>& x) {
    const size_t pad = x.size();
    std::vector<cf> y(pad, cf(0, 0));
    y.insert(y.end(), x.begin(), x.end());
    y.insert(y.end(), pad, cf(0, 0));
    return y;
}

std::vector<uint8_t> random_bits_u8(size_t n, std::mt19937& rng) {
    std::vector<uint8_t> b(n);
    for (auto& v : b) v = static_cast<uint8_t>(rng() & 1);
    return b;
}

}  // namespace

TEST_CASE("pulse-shaped BPSK is recognised as BPSK across symbol rates, roll-offs, carrier offsets and noise") {
    std::mt19937 rng(21);
    int hits = 0, total = 40;
    for (int t = 0; t < total; ++t) {
        const auto bits = random_bits_u8(6000, rng);
        std::vector<cf> sym(bits.size());
        for (size_t i = 0; i < bits.size(); ++i) sym[i] = bits[i] ? cf(1, 0) : cf(-1, 0);
        auto x = as_burst(shaped(sym, 4 + rng() % 13, 0.3f + 0.05f * (rng() % 10)));
        impair(x, 0.002 * (static_cast<int>(rng() % 11) - 5), 10 + rng() % 20, rng);
        hits += blind_linear_check(x).family == "bpsk";
    }
    CHECK(hits >= total * 9 / 10);
}

TEST_CASE("QPSK is recognised as QPSK, not BPSK") {
    std::mt19937 rng(22);
    int hits = 0, wrong = 0, total = 30;
    for (int t = 0; t < total; ++t) {
        const auto bits = random_bits_u8(12000, rng);
        std::vector<cf> sym(bits.size() / 2);
        const float a = 0.70710678f;
        for (size_t i = 0; i < sym.size(); ++i) sym[i] = cf(bits[2 * i] ? a : -a, bits[2 * i + 1] ? a : -a);
        auto x = as_burst(shaped(sym, 4 + rng() % 13, 0.3f + 0.05f * (rng() % 10)));
        impair(x, 0.002 * (static_cast<int>(rng() % 11) - 5), 12 + rng() % 18, rng);
        const auto r = blind_linear_check(x);
        hits += r.family == "qpsk";
        wrong += r.family == "bpsk";
    }
    CHECK(hits >= total * 8 / 10);
    CHECK(wrong == 0);
}

TEST_CASE("a continuous, steady-envelope linear signal is left alone: the check cannot tell it from FSK") {
    // Measured, and the reason for the envelope test's threshold: with no burst edges a shaped BPSK
    // signal's envelope varies by only 0.10-0.18, as much as noisy FSK's does. The check stays
    // silent rather than risk relabelling FSK; the classifier's label stands.
    std::mt19937 rng(25);
    const auto bits = random_bits_u8(6000, rng);
    std::vector<cf> sym(bits.size());
    for (size_t i = 0; i < bits.size(); ++i) sym[i] = bits[i] ? cf(1, 0) : cf(-1, 0);
    auto x = shaped(sym, 8, 0.5f);
    impair(x, 0.001, 20, rng);
    const auto r = blind_linear_check(x);
    CHECK(r.family.empty());
    CHECK(r.envelope_cv < 0.3);
}

TEST_CASE("real FSK is never relabelled: 300 FSK signals, narrow to wide, quiet to noisy") {
    std::mt19937 rng(23);
    int flagged = 0;
    for (int t = 0; t < 300; ++t) {
        const unsigned bps = 1 + rng() % 2;
        const unsigned sps = 8 + rng() % 24;
        const float bw = 0.04f + 0.01f * (rng() % 25);  // includes deviations narrow enough to put lines in the squared signal
        auto x = fsk_modulate(random_bits_u8(6000, rng), bps, sps, bw);
        impair(x, 0.001 * (static_cast<int>(rng() % 11) - 5), 5 + rng() % 26, rng);
        flagged += !blind_linear_check(x).family.empty();
    }
    CHECK(flagged == 0);
}

TEST_CASE("noise, tiny and degenerate inputs are inconclusive, never an error") {
    std::mt19937 rng(24);
    std::normal_distribution<float> g;
    std::vector<cf> noise(8192);
    for (auto& v : noise) v = cf(g(rng), g(rng));
    CHECK(blind_linear_check(noise).family.empty());
    CHECK(blind_linear_check(std::vector<cf>(10)).family.empty());
    CHECK(blind_linear_check(std::vector<cf>()).family.empty());
    CHECK(blind_linear_check(std::vector<cf>(5000, cf(0, 0))).family.empty());
}

// --- constellation check -------------------------------------------------------------------------

namespace {
// Random symbols of the named constellation, unit average power, with AWGN at `snr_db` and an
// arbitrary carrier phase.
std::vector<cf> symbol_stream(const std::string& name, size_t n, double snr_db, double phase, std::mt19937& rng) {
    std::vector<cf> pts;
    if (name == "bpsk") pts = {{1, 0}, {-1, 0}};
    else if (name == "qpsk") pts = {{1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
    else if (name == "psk8") for (int k = 0; k < 8; ++k) pts.push_back(std::polar(1.0f, static_cast<float>(k * M_PI / 4)));
    else {
        const int side = name == "qam16" ? 4 : 8;
        for (int i = 0; i < side; ++i)
            for (int q = 0; q < side; ++q) pts.push_back({static_cast<float>(2 * i - side + 1), static_cast<float>(2 * q - side + 1)});
    }
    double p = 0;
    for (auto& v : pts) p += std::norm(v);
    p /= static_cast<double>(pts.size());
    std::vector<cf> x(n);
    const double sigma = std::sqrt(1.0 / std::pow(10.0, snr_db / 10.0) / 2.0);
    std::normal_distribution<float> g;
    for (auto& v : x) {
        const cf s = pts[rng() % pts.size()] / static_cast<float>(std::sqrt(p));
        v = s * std::polar(1.0f, static_cast<float>(phase)) + cf(static_cast<float>(sigma) * g(rng), static_cast<float>(sigma) * g(rng));
    }
    return x;
}
}  // namespace

TEST_CASE("constellation check reads BPSK, QPSK and 8-PSK from fourth-order cumulants, at any carrier phase") {
    std::mt19937 rng(11);
    for (const char* name : {"bpsk", "qpsk", "psk8"}) {
        int hits = 0;
        for (int trial = 0; trial < 20; ++trial) {
            const auto x = symbol_stream(name, 800, 15.0, 0.37 * trial, rng);
            hits += constellation_check(x).best == name;
        }
        CHECK(hits >= 19);
    }
}

TEST_CASE("constellation check puts the QAM orders in the QAM family, never in a PSK") {
    std::mt19937 rng(12);
    for (const char* name : {"qam16", "qam64"}) {
        int qam = 0, exact = 0;
        for (int trial = 0; trial < 20; ++trial) {
            const auto x = symbol_stream(name, 4000, 25.0, 0.5, rng);
            const auto c = constellation_check(x);
            qam += (c.best == "qam16" || c.best == "qam64");
            exact += (c.best == name);
        }
        CHECK(qam >= 18);
        CHECK(exact >= 12);  // order itself is the hard part; see the header comment
    }
}

TEST_CASE("spectral peakiness separates symbol-rate linear data from oversampled FSK") {
    std::mt19937 rng(21);
    for (const char* name : {"bpsk", "qpsk", "psk8", "qam16", "qam64"})
        for (double snr : {25.0, 6.0}) CHECK(spectral_peakiness(symbol_stream(name, 600, snr, 0.3, rng)) < 7.5);
    int peaky = 0;
    for (int trial = 0; trial < 20; ++trial) {
        auto x = fsk_modulate(random_bits_u8(600, rng), 1, 8, 0.2f);
        peaky += spectral_peakiness(x) > 7.5;
    }
    CHECK(peaky >= 19);
    CHECK(spectral_peakiness({}) == 0.0);
}

TEST_CASE("constellation check is inconclusive on too little or empty input") {
    CHECK(constellation_check({}).best.empty());
    CHECK(constellation_check(std::vector<cf>(50, cf(1, 0))).best.empty());
    CHECK(constellation_check(std::vector<cf>(500, cf(0, 0))).best.empty());
}

TEST_MAIN()
