// Blind wideband channel detection and extraction (see core/include/channel_finder.hpp).
// The PS's own stated case ("signal collected off the air typically range from few Khz
// to Ghz bands... recorded from different sensors and different locations") includes a
// capture spanning more bandwidth than one signal occupies; this is the coarse first
// step of finding where a signal actually sits before the rest of the pipeline (which
// assumes it is already looking at one narrowband capture) can do anything with it.

#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

#include <liquid/liquid.h>

#include "channel_finder.hpp"
#include "demod.hpp"
#include "recover.hpp"
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

void mix_up(std::vector<cf>& x, double freq_hz, double sample_rate) {
    for (size_t i = 0; i < x.size(); ++i) {
        const float phase = 2.0f * static_cast<float>(M_PI) * static_cast<float>(freq_hz / sample_rate) *
                             static_cast<float>(i);
        x[i] *= cf(std::cos(phase), std::sin(phase));
    }
}

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

unsigned int bidirectional_bit_errors(const std::vector<uint8_t>& out, const std::vector<uint8_t>& want,
                                       size_t window) {
    unsigned int best = static_cast<unsigned int>(window);
    for (size_t off = 0; off + window <= out.size(); ++off) {
        unsigned int mism = 0, inv = 0;
        for (size_t i = 0; i < window; ++i) {
            const bool a = out[off + i] != 0, b = want[i] != 0;
            mism += (a != b);
            inv += ((!a) != b);
        }
        best = std::min({best, mism, inv});
    }
    for (size_t off = 1; off + window <= want.size(); ++off) {
        unsigned int mism = 0, inv = 0;
        for (size_t i = 0; i < window; ++i) {
            const bool a = out[i] != 0, b = want[off + i] != 0;
            mism += (a != b);
            inv += ((!a) != b);
        }
        best = std::min({best, mism, inv});
    }
    return best;
}

}  // namespace

TEST_CASE("find_channels: locates a single narrowband signal within a wideband capture") {
    const double sample_rate = 48000.0;
    const unsigned int sps = 24;
    const auto bits = random_bits(400, 1);
    const auto symbols = linear_modulate(bits, ModulationScheme::BPSK);
    auto shaped = rrc_pulses(symbols, sps);
    mix_up(shaped, /*freq_hz=*/12000.0, sample_rate);

    std::vector<cf> wideband(shaped.size());
    add_noise(wideband, 0.02f, 42);  // wideband noise floor across the whole capture first
    for (size_t i = 0; i < shaped.size(); ++i) wideband[i] += shaped[i];

    const auto result = find_channels(wideband, sample_rate);
    CHECK(!result.channels.empty());
    if (!result.channels.empty()) {
        const auto& c = result.channels.front();  // strongest
        CHECK(std::abs(c.center_freq_hz - 12000.0) < 500.0);
        CHECK(c.bandwidth_hz > 1000.0);   // a real occupied band, not a single-bin spike
        CHECK(c.bandwidth_hz < 10000.0);  // not the whole capture either
        CHECK(c.confidence > 0.9);        // the only real channel present
    }
}

TEST_CASE("find_channels: separates two signals at different frequencies") {
    const double sample_rate = 48000.0;
    const unsigned int sps = 24;
    auto a = rrc_pulses(linear_modulate(random_bits(400, 2), ModulationScheme::BPSK), sps);
    auto b = rrc_pulses(linear_modulate(random_bits(400, 3), ModulationScheme::BPSK), sps);
    mix_up(a, 15000.0, sample_rate);
    mix_up(b, -10000.0, sample_rate);

    std::vector<cf> wideband(a.size());
    add_noise(wideband, 0.02f, 43);
    for (size_t i = 0; i < wideband.size(); ++i) wideband[i] += a[i] + b[i];

    const auto result = find_channels(wideband, sample_rate);
    CHECK(result.channels.size() >= 2);

    bool found_a = false, found_b = false;
    for (const auto& c : result.channels) {
        if (std::abs(c.center_freq_hz - 15000.0) < 500.0) found_a = true;
        if (std::abs(c.center_freq_hz - (-10000.0)) < 500.0) found_b = true;
    }
    CHECK(found_a);
    CHECK(found_b);
}

TEST_CASE("find_channels: reports nothing for pure noise") {
    std::vector<cf> noise_only(8192);
    add_noise(noise_only, 1.0f, 99);
    const auto result = find_channels(noise_only, 48000.0);
    CHECK(result.channels.empty());
}

TEST_CASE("find_channels rejects degenerate input") {
    bool threw = false;
    try {
        find_channels({}, 48000.0);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);

    threw = false;
    try {
        find_channels(std::vector<cf>(1000, cf(1, 0)), 48000.0, /*nfft=*/1);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);

    threw = false;
    try {
        find_channels(std::vector<cf>(1000, cf(1, 0)), /*sample_rate=*/0.0);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

TEST_CASE("extract_channel: isolating a channel and decimating it recovers a real embedded message") {
    // The actual end-to-end claim: given a wideband capture with a real signal sitting
    // somewhere other than DC, find it, extract it to baseband at a lower sample rate,
    // and decode it with the exact same recover_linear this project already uses for a
    // real-world capture (see recover.hpp) -- not just "extract_channel runs without
    // throwing". The channel here is given directly (a known center/bandwidth), not
    // blind-detected -- find_channels' own detection accuracy is covered by the tests
    // above; this isolates extract_channel's own mixing/filtering/decimation.
    const double sample_rate = 48000.0;
    const double symbol_rate_hz = 2000.0;
    const unsigned int sps = static_cast<unsigned int>(sample_rate / symbol_rate_hz);  // 24
    const auto bits = random_bits(600, 7);
    const auto symbols = linear_modulate(bits, ModulationScheme::BPSK);
    auto shaped = rrc_pulses(symbols, sps);
    mix_up(shaped, /*freq_hz=*/12000.0, sample_rate);

    std::vector<cf> wideband(shaped.size());
    add_noise(wideband, 0.02f, 777);
    for (size_t i = 0; i < shaped.size(); ++i) wideband[i] += shaped[i];

    // bandwidth_hz chosen close to this signal's own real (RRC, rolloff 0.35)
    // analytical bandwidth (~1.35 * symbol_rate_hz = 2700 Hz) so that
    // sample_rate / (bandwidth_hz * 3.0) (extract_channel's own oversampling
    // margin) floors to exactly 6: decim=6 -> extracted sample rate 8000 Hz ->
    // exactly 4 samples/symbol at symbol_rate_hz=2000, a clean, generous value
    // for recover_linear's own symsync (test_recover.cpp uses the same sps for
    // its own RRC cases).
    ChannelCandidate channel;
    channel.center_freq_hz = 12000.0;
    channel.bandwidth_hz = 2600.0;
    channel.confidence = 1.0;

    const auto extracted = extract_channel(wideband, sample_rate, channel);
    CHECK(std::abs(extracted.sample_rate - 8000.0) < 1.0);
    const unsigned int sps_extracted = static_cast<unsigned int>(std::lround(extracted.sample_rate / symbol_rate_hz));
    CHECK(sps_extracted == 4);

    const auto recovered = recover_linear(extracted.samples, sps_extracted, ModulationScheme::BPSK, PulseShape::Rrc);
    const auto recovered_bits = linear_demodulate(recovered.samples, ModulationScheme::BPSK);
    CHECK(recovered_bits.size() > 400);

    const auto errors = bidirectional_bit_errors(recovered_bits, bits, 400);
    CHECK(errors < 400u / 20);  // under 5% wrong at the best alignment: a real decode
}

TEST_CASE("extract_channel rejects degenerate input") {
    ChannelCandidate ch;
    ch.center_freq_hz = 0.0;
    ch.bandwidth_hz = 1000.0;

    bool threw = false;
    try {
        extract_channel({}, 48000.0, ch);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);

    ChannelCandidate bad_bw = ch;
    bad_bw.bandwidth_hz = 0.0;
    threw = false;
    try {
        extract_channel(std::vector<cf>(1000, cf(1, 0)), 48000.0, bad_bw);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);

    ChannelCandidate too_wide = ch;
    too_wide.bandwidth_hz = 96000.0;  // >= sample_rate
    threw = false;
    try {
        extract_channel(std::vector<cf>(1000, cf(1, 0)), 48000.0, too_wide);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

TEST_MAIN()
