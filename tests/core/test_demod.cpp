// Phase 3: modulation -> demodulation round trips (PSK/QAM/ASK/OOK and FSK).
// Same assertions on the native and WASM builds; the golden hashes pin the
// modulator output so a WASM/native divergence shows up as a failure.

#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

#include "demod.hpp"
#include "../harness.hpp"
#include "../testutil.hpp"

using namespace sigcore;
using namespace sigtest;
using cf = std::complex<float>;

namespace {

struct Scheme {
    ModulationScheme scheme;
    unsigned int bps;
    const char* name;
};

const Scheme kSchemes[] = {
    {ModulationScheme::BPSK, 1, "BPSK"},   {ModulationScheme::QPSK, 2, "QPSK"},
    {ModulationScheme::PSK8, 3, "PSK8"},   {ModulationScheme::PSK16, 4, "PSK16"},
    {ModulationScheme::PSK32, 5, "PSK32"}, {ModulationScheme::PSK64, 6, "PSK64"},
    {ModulationScheme::QAM4, 2, "QAM4"},   {ModulationScheme::QAM16, 4, "QAM16"},
    {ModulationScheme::QAM32, 5, "QAM32"}, {ModulationScheme::QAM64, 6, "QAM64"},
    {ModulationScheme::QAM128, 7, "QAM128"}, {ModulationScheme::QAM256, 8, "QAM256"},
    {ModulationScheme::ASK2, 1, "ASK2"},   {ModulationScheme::ASK4, 2, "ASK4"},
    {ModulationScheme::ASK8, 3, "ASK8"},   {ModulationScheme::ASK16, 4, "ASK16"},
    {ModulationScheme::OOK, 1, "OOK"},
};

// Hash of samples quantized to 1e-3 (robust to last-bit libm differences).
uint32_t hash_samples(const std::vector<cf>& s) {
    std::vector<uint8_t> bytes;
    for (const auto& v : s) {
        for (float part : {v.real(), v.imag()}) {
            const int32_t q = static_cast<int32_t>(std::lround(part * 1000.0f));
            for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<uint8_t>(q >> (8 * i)));
        }
    }
    return fnv1a(bytes);
}

std::vector<cf> add_noise(std::vector<cf> s, float amplitude, uint64_t seed) {
    Rng rng(seed);
    for (auto& v : s) v += cf(amplitude * rng.uniform(), amplitude * rng.uniform());
    return s;
}

}  // namespace

TEST_CASE("linear modulate/demodulate round trip, all 17 schemes (noiseless)") {
    // 840 = lcm(1..8): a whole number of symbols for every bits-per-symbol.
    const auto bits = random_bits(840, 1);
    for (const auto& s : kSchemes) {
        const auto samples = linear_modulate(bits, s.scheme);
        CHECK(samples.size() == bits.size() / s.bps);
        const auto rec = linear_demodulate(samples, s.scheme);
        CHECK(rec.size() >= bits.size());
        CHECK(std::equal(bits.begin(), bits.end(), rec.begin()));
    }
}

TEST_CASE("linear modulation pads a partial final symbol with zeros") {
    const auto bits = random_bits(10, 2);  // 10 bits into 4-bit QAM16 symbols -> 3 symbols
    const auto samples = linear_modulate(bits, ModulationScheme::QAM16);
    CHECK(samples.size() == 3);
    const auto rec = linear_demodulate(samples, ModulationScheme::QAM16);
    CHECK(rec.size() == 12);
    CHECK(std::equal(bits.begin(), bits.end(), rec.begin()));
    CHECK(rec[10] == 0 && rec[11] == 0);
}

TEST_CASE("linear demodulation tolerates small additive noise") {
    const auto bits = random_bits(840, 3);
    for (auto scheme : {ModulationScheme::BPSK, ModulationScheme::QPSK, ModulationScheme::PSK8,
                        ModulationScheme::QAM16, ModulationScheme::ASK4}) {
        const auto noisy = add_noise(linear_modulate(bits, scheme), 0.05f, 4);
        const auto rec = linear_demodulate(noisy, scheme);
        CHECK(std::equal(bits.begin(), bits.end(), rec.begin()));
    }
}

TEST_CASE("bits_to_symbols / symbols_to_bits are inverses, MSB first") {
    const std::vector<uint8_t> bits = {1, 0, 1, 1, 0, 0, 1, 0, 1};  // 9 bits, bps=3
    const auto syms = bits_to_symbols(bits, 3);
    CHECK(syms.size() == 3);
    CHECK(syms[0] == 5 && syms[1] == 4 && syms[2] == 5);  // 101, 100, 101
    CHECK(symbols_to_bits(syms, 3) == bits);

    // Partial final symbol is zero padded.
    const auto padded = bits_to_symbols({1, 1, 1, 1, 1}, 4);
    CHECK(padded.size() == 2 && padded[0] == 15 && padded[1] == 8);
}

TEST_CASE("fsk modulate/demodulate round trip") {
    struct Case { unsigned int bps, sps; };
    for (const Case c : {Case{1, 8}, Case{2, 8}, Case{3, 16}, Case{4, 32}}) {
        const auto bits = random_bits(c.bps * 24, 10 + c.bps);
        const auto samples = fsk_modulate(bits, c.bps, c.sps, 0.25f);
        CHECK(samples.size() == (bits.size() / c.bps) * c.sps);
        CHECK(fsk_demodulate(samples, c.bps, c.sps, 0.25f) == bits);
    }
}

TEST_CASE("fsk demodulation tolerates small additive noise") {
    const auto bits = random_bits(96, 20);
    const auto noisy = add_noise(fsk_modulate(bits, 2, 8, 0.25f), 0.1f, 21);
    CHECK(fsk_demodulate(noisy, 2, 8, 0.25f) == bits);
}

TEST_CASE("demod rejects invalid input") {
    CHECK_THROWS(linear_modulate({}, ModulationScheme::BPSK));
    CHECK_THROWS(linear_demodulate({}, ModulationScheme::BPSK));
    CHECK_THROWS(fsk_modulate({}, 1, 8, 0.25f));
    CHECK_THROWS(fsk_demodulate({}, 1, 8, 0.25f));
    auto samples = fsk_modulate(random_bits(16, 5), 1, 8, 0.25f);
    samples.pop_back();  // no longer a whole number of symbols
    CHECK_THROWS(fsk_demodulate(samples, 1, 8, 0.25f));
    CHECK_THROWS(bits_to_symbols({1, 0}, 0));
    CHECK_THROWS(symbols_to_bits({1u}, 0));
}

// Golden hashes of the modulator output for a fixed 240-bit input (native run).
TEST_CASE("golden modulator output: QAM16") {
    CHECK_EQ(hash_samples(linear_modulate(random_bits(240, 99), ModulationScheme::QAM16)), 1168454280);
}
TEST_CASE("golden modulator output: PSK8") {
    CHECK_EQ(hash_samples(linear_modulate(random_bits(240, 99), ModulationScheme::PSK8)), 534221477);
}
TEST_CASE("golden modulator output: ASK4") {
    CHECK_EQ(hash_samples(linear_modulate(random_bits(240, 99), ModulationScheme::ASK4)), 513368722);
}
TEST_CASE("golden modulator output: FSK bps=2 sps=8") {
    CHECK_EQ(hash_samples(fsk_modulate(random_bits(240, 99), 2, 8, 0.25f)), 1569463537);
}

TEST_MAIN()
