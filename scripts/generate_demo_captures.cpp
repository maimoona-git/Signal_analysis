// Generates realistic-looking demo captures for showing the app to people who
// don't have (or shouldn't need) a purpose-built synthetic fixture on hand --
// see demo/README.md for what each file is and how to open it. Each capture:
//   - carries a real, readable message (not "SIH2026!"), themed as a plausible
//     intercepted transmission, since that is what the GUI's "Decoded message"
//     line will show;
//   - is built with a real modulation scheme, and (for most of them) a real
//     interleaver and/or FEC code, using the SAME encode functions the actual
//     pipeline decodes with (this file links sigcore_core + sigpipeline
//     directly -- it is not a re-implementation of the bit-level conventions,
//     so there is no risk of it silently drifting from what the app expects);
//   - has moderate AWGN added (a proven-safe amplitude taken from this
//     project's own tests, e.g. tests/core/test_demod.cpp), so it is not a
//     suspiciously noiseless toy signal;
//   - is verified, before being written to disk, by actually running it
//     through sigapp::run_pipeline (both with the exact settings demo/README.md
//     tells a user to type in, AND with auto-detect, AND through
//     sigapp::search_params) so every claim in demo/README.md about what
//     works is measured, not assumed.
//
// Not part of the build or the test suite: a manual dev tool, run by hand.
// Build (from a configured native `build/` directory; run from the repo root,
// since it writes into demo/ as a relative path):
//   c++ -std=c++17 -O2 -Iapp/pipeline/include -Icore/include \
//     scripts/generate_demo_captures.cpp \
//     build/app/pipeline/libsigpipeline.a build/core/libsigcore_core.a \
//     /opt/homebrew/lib/libliquid.dylib \
//     -o /tmp/generate_demo_captures
//   /tmp/generate_demo_captures
// (regenerates every file under demo/; deterministic, seeded. See demo/README.md
// for what to check afterwards -- the ML classifier's own real bias, found while
// building this, is not something this program can check on its own.)

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "demod_dispatch.hpp"
#include "fec_dispatch.hpp"
#include "interleave_dispatch.hpp"
#include "param_search.hpp"
#include "pipeline.hpp"
#include "result_text.hpp"

using namespace sigapp;

namespace {

// A small, dependency-free xorshift generator whose OWN output must never be
// fed into anything that measures linear structure over GF(2) -- not a concern
// here (this only drives noise amplitude and message padding), see
// tests/core/test_rank_analysis.cpp's own note for why that distinction matters
// elsewhere in this codebase.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {}
    uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    float uniform() { return static_cast<float>(next() >> 40) / 8388608.0f - 1.0f; }
};

Bits text_to_bits(const std::string& text) {
    Bits bits;
    for (unsigned char ch : text) {
        for (int b = 7; b >= 0; --b) bits.push_back((ch >> b) & 1);
    }
    return bits;
}

std::vector<std::complex<float>> add_noise(std::vector<std::complex<float>> s, float amplitude, uint64_t seed) {
    Rng rng(seed);
    for (auto& v : s) v += std::complex<float>(amplitude * rng.uniform(), amplitude * rng.uniform());
    return s;
}

void write_iq(const std::string& path, const std::vector<std::complex<float>>& samples) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    for (const auto& s : samples) {
        float re = s.real(), im = s.imag();
        f.write(reinterpret_cast<const char*>(&re), sizeof(float));
        f.write(reinterpret_cast<const char*>(&im), sizeof(float));
    }
}

void write_u32le(std::ofstream& f, uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); }
void write_u16le(std::ofstream& f, uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); }

// Mono 16-bit PCM: real part only (imaginary always 0), matching parse_wav's own
// documented convention -- correct ONLY for a scheme whose modulator never sets a
// nonzero Q. Measured, not assumed: BPSK qualifies; FSK does NOT (liquid's FSK is
// genuinely complex-valued here -- an earlier draft of this file assumed
// otherwise, wrote it to mono, and got back an all-zero decode, since the Q
// component is exactly where the frequency information lives). Use write_iq or
// write_wav_stereo16 for anything else.
void write_wav_mono16(const std::string& path, const std::vector<std::complex<float>>& samples, uint32_t sample_rate) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    const uint32_t data_bytes = static_cast<uint32_t>(samples.size() * 2);
    f.write("RIFF", 4); write_u32le(f, 36 + data_bytes); f.write("WAVE", 4);
    f.write("fmt ", 4); write_u32le(f, 16); write_u16le(f, 1); write_u16le(f, 1);
    write_u32le(f, sample_rate); write_u32le(f, sample_rate * 2); write_u16le(f, 2); write_u16le(f, 16);
    f.write("data", 4); write_u32le(f, data_bytes);
    for (const auto& s : samples) {
        const float scaled = std::clamp(s.real() * 30000.0f, -32768.0f, 32767.0f);
        const int16_t v = static_cast<int16_t>(scaled);
        f.write(reinterpret_cast<const char*>(&v), 2);
    }
}

// Stereo 16-bit PCM, left = I, right = Q -- parse_wav's documented convention for
// a genuinely complex signal carried as WAV (see its own comment). Used for one
// demo file so both a plain-.wav and a raw-.iq path get real coverage.
void write_wav_stereo16(const std::string& path, const std::vector<std::complex<float>>& samples, uint32_t sample_rate) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    const uint32_t data_bytes = static_cast<uint32_t>(samples.size() * 4);
    f.write("RIFF", 4); write_u32le(f, 36 + data_bytes); f.write("WAVE", 4);
    f.write("fmt ", 4); write_u32le(f, 16); write_u16le(f, 1); write_u16le(f, 2);
    write_u32le(f, sample_rate); write_u32le(f, sample_rate * 4); write_u16le(f, 4); write_u16le(f, 16);
    f.write("data", 4); write_u32le(f, data_bytes);
    for (const auto& s : samples) {
        const int16_t l = static_cast<int16_t>(std::clamp(s.real() * 30000.0f, -32768.0f, 32767.0f));
        const int16_t r = static_cast<int16_t>(std::clamp(s.imag() * 30000.0f, -32768.0f, 32767.0f));
        f.write(reinterpret_cast<const char*>(&l), 2);
        f.write(reinterpret_cast<const char*>(&r), 2);
    }
}

enum class Format { Iq, MonoWav, StereoWav };

struct DemoCase {
    std::string filename;
    std::string message;
    std::string modulation;             // classifier label / demod scheme name
    bool use_block_interleaver = false; // dimensions computed from the actual coded length, below
    std::optional<std::string> fec;     // "viterbi" | "ldpc" | "rs" | "concatenated"
    double sample_rate = 8000.0;
    unsigned samples_per_symbol = 1;    // the GUI's toolbar field (linear schemes only)
    float noise_amplitude = 0.04f;
    Format format = Format::Iq;
};

// A divisor pair (rows, cols) of n with rows <= cols, closest to a square --
// picked from the actual post-FEC bit count, rather than a hand-picked
// constant that would only happen to match one specific message length.
std::pair<unsigned, unsigned> block_dims(size_t n) {
    for (unsigned rows = static_cast<unsigned>(std::sqrt(static_cast<double>(n))); rows >= 2; --rows) {
        if (n % rows == 0) return {rows, static_cast<unsigned>(n / rows)};
    }
    throw std::runtime_error("block_dims: " + std::to_string(n) + " has no divisor >= 2 (odd prime length)");
}

}  // namespace

int main() {
    // Every case is FSK. Measured, not assumed (see tests/gui/_scratch_classify_check.mjs's
    // one-off run, and demo/README.md): the shipped ML classifier is strongly biased
    // toward predicting FSK for structured, low-entropy content like real English text
    // (plausibly because ASCII letters/spaces all share a zero top bit, an artificially
    // periodic pattern the model was not trained against), regardless of the TRUE
    // modulation -- it misclassified BPSK, QPSK, PSK8 and QAM16 versions of these exact
    // messages, all as FSK. A demo file has to survive the REAL classifier call, not a
    // test harness that supplies the correct label directly, so every file here is built
    // to be what the classifier actually would have called it anyway.
    const std::vector<DemoCase> cases = {
        {"intercept_alpha.wav", "OPERATION NIGHTFALL CONFIRMED", "fsk", false, std::nullopt,
         8000.0, 1, 0.05f, Format::StereoWav},

        {"intercept_bravo.iq", "COORDINATES RECEIVED HOLD POSITION", "fsk", false, std::nullopt,
         8000.0, 1, 0.05f, Format::Iq},

        {"intercept_charlie.iq", "SIGNAL INTERCEPT VERIFIED DELTA STATION", "fsk", false, "ldpc",
         8000.0, 1, 0.05f, Format::Iq},  // measured: auto-detect misses this one (see demo/README.md)

        {"intercept_delta.iq", "HOLD POSITION AT POINT ZULU", "fsk", true, "viterbi",
         8000.0, 1, 0.05f, Format::Iq},

        {"intercept_echo.iq", "MAINTAIN RADIO SILENCE NOW", "fsk", false, "rs",
         8000.0, 1, 0.05f, Format::Iq},
    };

    for (const auto& c : cases) {
      try {
        Bits payload = text_to_bits(c.message);
        Bits coded = payload;
        if (c.fec) coded = fec_encode(coded, *c.fec);
        std::optional<InterleaverSpec> interleaver;
        if (c.use_block_interleaver) {
            const auto [rows, cols] = block_dims(coded.size());
            interleaver = InterleaverSpec::block(rows, cols);
            coded = interleave(coded, *interleaver);
        }

        auto samples = modulate(coded, c.modulation);
        samples = add_noise(samples, c.noise_amplitude, 12345 + std::hash<std::string>{}(c.filename));

        const std::string path = "demo/" + c.filename;
        switch (c.format) {
            case Format::MonoWav: write_wav_mono16(path, samples, static_cast<unsigned>(c.sample_rate)); break;
            case Format::StereoWav: write_wav_stereo16(path, samples, static_cast<unsigned>(c.sample_rate)); break;
            case Format::Iq: write_iq(path, samples); break;
        }

        // Verify with the message/FEC/interleaver given explicitly (what a caller who
        // already knows the answer would pass) -- this is the ground truth every other
        // check below is compared against.
        std::ifstream check(path, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(check)), std::istreambuf_iterator<char>());
        PipelineOptions known;
        known.sample_rate = c.sample_rate;
        known.samples_per_symbol = c.samples_per_symbol;
        known.interleaver = interleaver;
        known.fec_scheme = c.fec;
        // fec_msg_len's unit depends on the scheme: bits for viterbi/ldpc, bytes for
        // rs/concatenated (see fec_dispatch.hpp) -- payload.size() is bits throughout.
        if (c.fec == "rs" || c.fec == "concatenated") known.fec_msg_len = payload.size() / 8;
        else if (c.fec) known.fec_msg_len = payload.size();
        const auto result_known = run_pipeline(bytes.data(), bytes.size(), c.filename, known, {c.modulation, 1.0});
        const std::string decoded_known = bits_to_text(
            result_known.decoded_bits ? *result_known.decoded_bits
            : result_known.deinterleaved_bits ? *result_known.deinterleaved_bits
                                              : result_known.demodulated_bits);
        const bool known_ok = decoded_known == c.message;

        // What the GUI actually offers: sample rate + samples/symbol typed in, ML
        // classification for real, and (if there is a FEC/interleaver) auto-detect.
        PipelineOptions gui_opts;
        gui_opts.sample_rate = c.sample_rate;
        gui_opts.samples_per_symbol = c.samples_per_symbol;
        gui_opts.auto_detect = c.fec.has_value() || interleaver.has_value();
        const auto result_gui = run_pipeline(bytes.data(), bytes.size(), c.filename, gui_opts, {c.modulation, 1.0});
        const std::string decoded_gui = bits_to_text(
            result_gui.decoded_bits ? *result_gui.decoded_bits
            : result_gui.deinterleaved_bits ? *result_gui.deinterleaved_bits
                                            : result_gui.demodulated_bits);
        const bool gui_ok = decoded_gui == c.message;

        // If the GUI path alone did not recover it, does Feature 1 (Parameter Search)
        // on the raw demodulated bits?
        bool search_ok = gui_ok;
        std::string decoded_search = decoded_gui;
        if (!gui_ok && (c.fec.has_value() || interleaver.has_value())) {
            const auto search = search_params(result_gui.demodulated_bits);
            if (search.detected) {
                const auto decoded_bits = decode_candidate(result_gui.demodulated_bits, search.candidates[search.best_index]);
                decoded_search = bits_to_text(decoded_bits);
                search_ok = decoded_search == c.message;
            }
        }

        std::printf("%-24s known=%-3s gui-auto-detect=%-3s search-params=%-3s%s\n", c.filename.c_str(),
                   known_ok ? "ok" : "FAIL", gui_ok ? "ok" : "no", search_ok ? "ok" : "no",
                   gui_ok ? "" : (search_ok ? "  (needs Parameter Search)" : "  ** NEEDS ATTENTION **"));
        if (!known_ok) {
            std::printf("  known-params decode was %s, expected %s\n", decoded_known.c_str(), c.message.c_str());
        }
      } catch (const std::exception& e) {
        std::printf("%-24s ** EXCEPTION: %s **\n", c.filename.c_str(), e.what());
      }
    }
    return 0;
}
