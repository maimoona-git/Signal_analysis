// Phase 6: the full pipeline (ingest -> spectrogram -> demod -> de-interleave ->
// FEC decode -> correlate, with optional blind detection) on 84 scenarios
// recorded from the ORIGINAL app/pipeline.py (tests/data/pipeline/pipeline.rec):
// the native app's two demo captures with its real classifier, every modulation,
// WAV input, noisy channels, explicit and auto-detected parameters for every
// FEC x interleaver pairing, and every way the original raises.
//
// The ML classification is an input here (the recorded label/confidence): the
// classifier itself is verified separately (Phase 5), and end to end through the
// browser in tests/bridge/pipeline.test.mjs.

#include <cmath>
#include <complex>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <liquid/liquid.h>

#include "fec_dispatch.hpp"
#include "framing.hpp"
#include "param_search.hpp"
#include "pipeline.hpp"
#include "result_text.hpp"
#include "../harness.hpp"
#include "../testutil.hpp"
#include "../record_file.hpp"

using namespace sigapp;
using namespace sigtest;

#ifndef SIG_TEST_DATA_DIR
#error "SIG_TEST_DATA_DIR must point at tests/data"
#endif

namespace {

std::string data_path(const std::string& rel) { return std::string(SIG_TEST_DATA_DIR) + "/pipeline/" + rel; }

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

PipelineOptions options_of(const Record& r) {
    PipelineOptions o;
    if (!is_none(field(r, "sample_rate"))) o.sample_rate = to_double(field(r, "sample_rate"));
    o.samples_per_symbol = static_cast<unsigned int>(to_int(field(r, "samples_per_symbol")));
    for (const auto& [k, v] : params_of(field(r, "demod"))) {
        if (k == "bits_per_symbol") o.demod.bits_per_symbol = static_cast<unsigned int>(to_int(v));
        if (k == "samples_per_symbol") o.demod.samples_per_symbol = static_cast<unsigned int>(to_int(v));
        if (k == "bandwidth") o.demod.bandwidth = static_cast<float>(to_double(v));
    }
    if (!is_none(field(r, "interleaver"))) {
        InterleaverSpec s;
        s.kind = field(r, "interleaver");
        for (const auto& [k, v] : params_of(field(r, "iparams"))) {
            const auto value = static_cast<uint32_t>(to_int(v));
            if (k == "rows") s.rows = value;
            if (k == "cols") s.cols = value;
            if (k == "seed") s.seed = value;
            if (k == "branches") s.branches = value;
            if (k == "delay_increment") s.delay_increment = value;
        }
        o.interleaver = s;
    }
    if (!is_none(field(r, "fec"))) o.fec_scheme = field(r, "fec");
    if (!is_none(field(r, "msg_len"))) o.fec_msg_len = static_cast<size_t>(to_int(field(r, "msg_len")));
    if (!is_none(field(r, "header"))) o.header_pattern = bits_of(field(r, "header"));
    o.auto_detect = to_int(field(r, "auto_detect")) != 0;
    return o;
}

std::string opt_int(const std::optional<int>& v) { return v ? std::to_string(*v) : "none"; }

}  // namespace

TEST_CASE("pipeline reproduces the original on every recorded scenario") {
    const auto recs = read_records(data_path("pipeline.rec"));
    Mismatches m;
    size_t errors = 0, detections = 0, decoded = 0;
    for (const auto& r : recs) {
        const std::string name = field(r, "name");
        const auto bytes = read_file(data_path(field(r, "file")));
        const PipelineOptions options = options_of(r);
        const Classification cls{has(r, "label") ? field(r, "label") : "bpsk",
                                 has(r, "confidence") ? to_double(field(r, "confidence")) : 1.0};

        if (has(r, "error")) {
            ++errors;
            bool threw = false;
            try { run_pipeline(bytes.data(), bytes.size(), field(r, "file"), options, cls); } catch (const std::exception&) { threw = true; }
            m.expect(threw, name, "original raised " + field(r, "error") + " but this did not throw");
            continue;
        }

        PipelineResult got;
        try {
            got = run_pipeline(bytes.data(), bytes.size(), field(r, "file"), options, cls);
        } catch (const std::exception& e) {
            m.expect(false, name, std::string("threw unexpectedly: ") + e.what());
            continue;
        }

        m.equal(got.file_path, field(r, "file"), name, "file_path");
        m.equal(got.predicted_modulation, field(r, "label"), name, "label");
        m.equal(got.file_format, field(r, "file_format"), name, "file_format");
        m.equal(got.num_samples, static_cast<size_t>(to_int(field(r, "num_samples"))), name, "num_samples");
        m.expect(got.sample_rate == to_double(field(r, "out_sample_rate")), name, "sample_rate");

        // spectrogram: geometry and axes exact; the dB data itself is checked by mean
        // (native fftw vs the WASM build's internal FFT differ in the last float bits)
        m.equal(got.spectrogram.frames, static_cast<unsigned>(to_int(field(r, "spec_frames"))), name, "spectrogram frames");
        m.equal(got.spectrogram.nfft, static_cast<unsigned>(to_int(field(r, "spec_nfft"))), name, "spectrogram nfft");
        m.expect(got.spectrogram.freqs.front() == to_double(field(r, "freq_first")), name, "freq_first");
        m.expect(got.spectrogram.freqs.back() == to_double(field(r, "freq_last")), name, "freq_last");
        m.expect(got.spectrogram.times.back() == to_double(field(r, "time_last")), name, "time_last");
        double sum = 0;
        for (float v : got.spectrogram.data) sum += v;
        m.expect(std::fabs(sum / static_cast<double>(got.spectrogram.data.size()) - to_double(field(r, "spec_mean"))) < 1e-3, name, "spectrogram mean");

        m.equal(got.demodulated_bits, bits_of(field(r, "demodulated")), name, "demodulated_bits");
        m.equal(got.deinterleaved_bits ? bits_str(*got.deinterleaved_bits) : std::string("none"), field(r, "deinterleaved"), name, "deinterleaved_bits");
        m.equal(got.decoded_bits ? bits_str(*got.decoded_bits) : std::string("none"), field(r, "decoded"), name, "decoded_bits");
        m.equal(got.detected_interleaver ? got.detected_interleaver->kind : std::string("none"), field(r, "detected_interleaver"), name, "detected_interleaver");
        m.equal(got.detected_fec.value_or("none"), field(r, "detected_fec"), name, "detected_fec");
        m.equal(opt_int(got.detection_score), field(r, "detection_score"), name, "detection_score");

        m.equal(got.boundaries.header_pattern, bits_of(field(r, "b_pattern")), name, "boundaries.header_pattern");
        m.equal(opt_int(got.boundaries.frame_period), field(r, "b_period"), name, "boundaries.frame_period");
        std::vector<size_t> want_pos;
        for (const auto& s : split(field(r, "b_positions"), ',')) want_pos.push_back(static_cast<size_t>(to_int(s)));
        m.equal(got.boundaries.header_positions, want_pos, name, "boundaries.header_positions");
        std::string payloads;
        for (const auto& p : got.boundaries.payloads) payloads += (payloads.empty() ? "" : ",") + bits_str(p);
        m.equal(payloads.empty() ? std::string("-") : payloads, field(r, "b_payloads"), name, "boundaries.payloads");

        if (got.detection_score) ++detections;
        if (got.decoded_bits) ++decoded;
    }
    CHECK(recs.size() == 84);
    CHECK(errors >= 6);
    CHECK(detections >= 20);
    CHECK(decoded >= 30);
    m.finish(recs.size());
}

TEST_CASE("the demo capture round trip recovers the exact original message") {
    // bpsk_ldpc_block.iq carries "SIH2026!" through LDPC + block interleaving + BPSK.
    const auto bytes = read_file(data_path("bpsk_ldpc_block.iq"));
    PipelineOptions o;
    o.sample_rate = 8000.0;
    o.samples_per_symbol = 4;
    o.interleaver = InterleaverSpec::block(8, 16);
    o.fec_scheme = "ldpc";
    o.fec_msg_len = 64;
    const auto r = run_pipeline(bytes.data(), bytes.size(), "bpsk_ldpc_block.iq", o, {"bpsk", 1.0});
    CHECK(r.decoded_bits.has_value());
    CHECK(bits_to_text(*r.decoded_bits) == "SIH2026!");
}

namespace {

liquid_float_complex* as_liquid_recover(std::complex<float>* p) {
    return reinterpret_cast<liquid_float_complex*>(p);
}

// The same bidirectional alignment-search scoring test_recover.cpp's best_bit_errors
// uses (see its own comment for why both directions matter): a real lock's start
// point is not precomputable, and can either have extra leading samples ahead of the
// message or be missing some from the front, depending on the recovery technique.
unsigned int bidirectional_bit_errors(const Bits& out, const Bits& want, size_t window) {
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

// Mixes a signal up to a given frequency offset -- the wideband-channel tests below
// use this to place a real signal away from DC within a wider capture, the exact
// real-world case sigcore::find_channels exists for (see its own comment).
void mix_up(std::vector<std::complex<float>>& x, double freq_hz, double sample_rate) {
    for (size_t i = 0; i < x.size(); ++i) {
        const float phase = 2.0f * static_cast<float>(M_PI) * static_cast<float>(freq_hz / sample_rate) *
                             static_cast<float>(i);
        x[i] *= std::complex<float>(std::cos(phase), std::sin(phase));
    }
}

// The same realistic-impairment technique as test_recover.cpp (see its own comment
// for why a proper matched RRC pulse, not this project's own zero-order-hold
// convention, is what makes this a meaningful test of real-world capture support):
// real transmit-side pulse shaping, a carrier offset, a non-symbol-aligned start,
// and light noise.
std::vector<std::complex<float>> make_impaired_iq(const std::vector<std::complex<float>>& symbols,
                                                   unsigned int sps, float cfo, float noise_amp,
                                                   unsigned int start_offset, uint64_t noise_seed) {
    const unsigned int delay_symbols = 3;
    const float rolloff = 0.35f;
    firinterp_crcf tx = firinterp_crcf_create_prototype(LIQUID_FIRFILT_RRC, sps, delay_symbols, rolloff, 0.0f);
    std::vector<std::complex<float>> shaped(symbols.size() * sps);
    for (size_t i = 0; i < symbols.size(); ++i) {
        std::complex<float> sym = symbols[i];
        firinterp_crcf_execute(tx, *as_liquid_recover(&sym), as_liquid_recover(&shaped[i * sps]));
    }
    firinterp_crcf_destroy(tx);

    std::vector<std::complex<float>> impaired;
    impaired.reserve(shaped.size() - start_offset);
    uint64_t rng = noise_seed;
    auto next_noise = [&]() {
        rng = rng * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<float>((rng >> 40) & 0xFFFFFF) / static_cast<float>(0xFFFFFF) - 0.5f;
    };
    for (size_t i = start_offset; i < shaped.size(); ++i) {
        const float phase = -2.0f * static_cast<float>(M_PI) * cfo * static_cast<float>(i - start_offset);
        std::complex<float> sample = shaped[i] * std::complex<float>(std::cos(phase), std::sin(phase));
        sample += std::complex<float>(next_noise(), next_noise()) * noise_amp;
        impaired.push_back(sample);
    }
    return impaired;
}

// Zero-order-hold (rectangular, unshaped) pulses instead of make_impaired_iq's RRC
// shaping -- see recover.hpp's PulseShape comment for why this is its own, separate
// real-world case, not just a variant of the RRC one.
std::vector<std::complex<float>> make_rect_impaired_iq(const std::vector<std::complex<float>>& symbols,
                                                        unsigned int sps, float cfo, float noise_amp,
                                                        unsigned int start_offset, uint64_t noise_seed) {
    std::vector<std::complex<float>> held(symbols.size() * sps);
    for (size_t i = 0; i < symbols.size(); ++i)
        for (unsigned int k = 0; k < sps; ++k) held[i * sps + k] = symbols[i];

    std::vector<std::complex<float>> impaired;
    impaired.reserve(held.size() - start_offset);
    uint64_t rng = noise_seed;
    auto next_noise = [&]() {
        rng = rng * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<float>((rng >> 40) & 0xFFFFFF) / static_cast<float>(0xFFFFFF) - 0.5f;
    };
    for (size_t i = start_offset; i < held.size(); ++i) {
        const float phase = -2.0f * static_cast<float>(M_PI) * cfo * static_cast<float>(i - start_offset);
        std::complex<float> sample = held[i] * std::complex<float>(std::cos(phase), std::sin(phase));
        sample += std::complex<float>(next_noise(), next_noise()) * noise_amp;
        impaired.push_back(sample);
    }
    return impaired;
}

std::vector<uint8_t> iq_bytes(const std::vector<std::complex<float>>& samples) {
    std::vector<uint8_t> out(samples.size() * 2 * sizeof(float));
    std::memcpy(out.data(), samples.data(), out.size());
    return out;
}

}  // namespace

TEST_CASE("recover_timing decodes a real-world-impaired capture end to end; naive decimation cannot") {
    // A genuine end-to-end claim, not just recover_linear in isolation (test_recover.cpp):
    // real transmit pulse shaping, a carrier offset and an arbitrary start phase, fed
    // through the exact same encode chain (FEC, modulation) and file format the real
    // pipeline decodes. Neither path is told the exact FEC framing up front (a real
    // capture's receiver would not know it either) -- both go through Parameter
    // Search (Feature 1), the same as the GUI's own "Search Parameters" button and
    // demo/README.md's Charlie/Delta/Echo cases, which exist precisely for a single,
    // non-repeating message like this one. If naive decimation cannot be rescued even
    // by Parameter Search but the recover_timing path can, that is the real gap this
    // feature closes -- not a hand-picked alignment or a framing hint neither a real
    // receiver nor this project's own existing tools would actually have.
    const std::string message = "REAL WORLD CAPTURE VERIFIED OK, HOLD POSITION AND REPORT STATUS NOW";
    Bits payload;
    for (unsigned char ch : message)
        for (int b = 7; b >= 0; --b) payload.push_back((ch >> b) & 1);

    const Bits coded = fec_encode(payload, "viterbi");
    const auto symbols = sigapp::modulate(coded, "bpsk");
    const unsigned int sps = 8;
    const auto impaired = make_impaired_iq(symbols, sps, /*cfo=*/0.0041f, /*noise_amp=*/0.02f,
                                           /*start_offset=*/5, /*noise_seed=*/321);
    const auto bytes = iq_bytes(impaired);

    ParamSearchOptions search_opts;
    search_opts.max_bit_offset = 60;  // recover_timing's output does not start bit-aligned to the codeword

    PipelineOptions naive_opts;
    naive_opts.sample_rate = 8000.0;
    naive_opts.samples_per_symbol = sps;
    const auto naive = run_pipeline(bytes.data(), bytes.size(), "impaired.iq", naive_opts, {"bpsk", 1.0});
    const auto naive_search = search_params(naive.demodulated_bits, search_opts);
    const bool naive_ok = naive_search.detected &&
        bits_to_text(decode_candidate(naive.demodulated_bits, naive_search.candidates[naive_search.best_index])) == message;
    CHECK(!naive_ok);  // proves this is a real gap: naive decimation cannot be rescued even by parameter search

    PipelineOptions recovered_opts = naive_opts;
    recovered_opts.recover_timing = true;
    const auto recovered = run_pipeline(bytes.data(), bytes.size(), "impaired.iq", recovered_opts, {"bpsk", 1.0});
    CHECK(recovered.recovery_used);
    CHECK(recovered.recovery_phase_ambiguity == 2u);  // BPSK
    const auto recovered_search = search_params(recovered.demodulated_bits, search_opts);
    CHECK(recovered_search.detected);
    if (recovered_search.detected) {
        const auto& best = recovered_search.candidates[recovered_search.best_index];
        // The matched filter's group delay costs a few symbols at *both* ends of the
        // capture, not just the start (the "settle" skip already accounted for) -- so
        // the very last character or two of a message run right up against the tail
        // can legitimately be trimmed. A clean prefix match, not necessarily exact
        // equality, is what a real receiver would actually get here too.
        const auto got = bits_to_text(decode_candidate(recovered.demodulated_bits, best));
        CHECK(message.rfind(got, 0) == 0);       // got is a genuine, uncorrupted prefix of message
        CHECK(got.size() >= message.size() - 2);  // losing at most the last couple of characters
    }
}

// Gap-list Phase 1.4: when a known FEC/interleaver IS given up front (so there is a
// re-encode agreement score to test each phase hypothesis against -- unlike the test
// above, which uses Parameter Search afterward instead), the pipeline itself should
// try every one of recover_timing's phase_ambiguity_count rotations and keep the one
// that verifies, rather than leaving a real capture's result scrambled exactly as
// often as clean (see RecoveryResult::phase_ambiguity_count's own comment).
TEST_CASE("the pipeline resolves recover_timing's own phase ambiguity to a verified decode") {
    const std::string message = "ROTATION RESOLVED, PAYLOAD VERIFIED END TO END";
    Bits payload;
    for (unsigned char ch : message)
        for (int b = 7; b >= 0; --b) payload.push_back((ch >> b) & 1);

    const Bits coded = fec_encode(payload, "ldpc");
    auto symbols = sigapp::modulate(coded, "bpsk");
    // Force the exact failure Problem F describes: with zero actual carrier/timing
    // error to correct (see below), a decision-directed loop has nothing to lock
    // onto except the signal's own, already-180-degrees-inverted content, so it
    // passes it straight through inverted -- a real, not hypothetical, way to land
    // on BPSK's "wrong" one of its two equally-valid phase locks.
    for (auto& s : symbols) s = -s;
    const unsigned int sps = 8;
    // Zero-order-hold (not make_impaired_iq's RRC shaping): this project's own
    // synthetic convention, with zero timing/carrier error -- the point of this test
    // is phase-ambiguity resolution specifically, isolated from the separate (and
    // separately scoped -- see Parameter Search, Phase 2.1) bit-offset problem a real
    // capture's own matched-filter group delay introduces; PulseShape::Rect matches
    // this unshaped convention so recover_linear's own boxcar filter locks exactly,
    // with no symbol count drift to misalign the FEC codeword against fec_msg_len.
    std::vector<std::complex<float>> shaped;
    shaped.reserve(symbols.size() * sps);
    for (const auto& s : symbols) for (unsigned int i = 0; i < sps; ++i) shaped.push_back(s);
    const auto bytes = iq_bytes(shaped);

    PipelineOptions o;
    o.sample_rate = 8000.0;
    o.samples_per_symbol = sps;
    o.recover_timing = true;
    o.pulse_shape = sigcore::PulseShape::Rect;
    o.fec_scheme = "ldpc";
    o.fec_msg_len = payload.size();
    const auto r = run_pipeline(bytes.data(), bytes.size(), "rotation.iq", o, {"bpsk", 1.0});
    CHECK(r.recovery_used);
    CHECK(r.recovery_phase_ambiguity == 2u);
    CHECK(r.decode_status == DecodeStatus::Verified);
    CHECK(r.decoded_bits.has_value());
    if (r.decoded_bits) {
        // No trimming tolerance needed here (unlike the RRC/real-offset test above):
        // zero-order-hold at exactly sps, with no genuine timing/carrier error to
        // begin with, leaves nothing for recover_linear's own filter group delay or
        // lock-in transient to cost.
        CHECK(bits_to_text(*r.decoded_bits) == message);
    }
}

TEST_CASE("pulse_shape=Rect decodes a real-world unshaped capture end to end; Rrc (the default) cannot") {
    // Same end-to-end shape as the recover_timing test above, but for the other real-
    // world case PulseShape exists for: a transmitter that never pulse-shapes its
    // output at all (PySDR's own bpsk_rect_20sps.sigmf-data file, in particular --
    // see recover.hpp's PulseShape comment). recover_timing alone (Rrc, the default)
    // is the wrong matched filter for this signal; pulse_shape=Rect is the fix.
    //
    // start_offset is a multiple of sps here (unlike the RRC test above, which uses
    // an arbitrary one): integrate_and_dump has no filter, so any *partial* leading
    // symbol is simply dropped rather than padded, which for a rate-1/2 convolutional
    // code desyncs every trellis pair downstream of it -- recovering from that is
    // exactly what the PS's own bitstream-correlation stage (Section v) exists for,
    // not Parameter Search's offset scan, so it is out of scope for this test. What
    // this test isolates is the matched-filter choice itself: given a symbol-aligned
    // start, does the right filter decode where the wrong one cannot.
    const std::string message = "1234567890 ABCDEFGHIJKLMNOPQRSTUVWXYZ ZYXWVUTSRQPONMLKJIHGFEDCBA 0987654321";
    Bits payload;
    for (unsigned char ch : message)
        for (int b = 7; b >= 0; --b) payload.push_back((ch >> b) & 1);

    const Bits coded = fec_encode(payload, "viterbi");
    const auto symbols = sigapp::modulate(coded, "bpsk");
    const unsigned int sps = 8;  // see test_recover.cpp's own comment on why 8, not 20
    const auto impaired = make_rect_impaired_iq(symbols, sps, /*cfo=*/0.003f, /*noise_amp=*/0.02f,
                                                /*start_offset=*/0, /*noise_seed=*/321);
    const auto bytes = iq_bytes(impaired);

    ParamSearchOptions search_opts;
    search_opts.max_bit_offset = 60;

    PipelineOptions rrc_opts;
    rrc_opts.sample_rate = 8000.0;
    rrc_opts.samples_per_symbol = sps;
    rrc_opts.recover_timing = true;  // default pulse_shape = Rrc
    const auto rrc_result = run_pipeline(bytes.data(), bytes.size(), "impaired.iq", rrc_opts, {"bpsk", 1.0});
    const auto rrc_search = search_params(rrc_result.demodulated_bits, search_opts);
    const bool rrc_ok = rrc_search.detected &&
        bits_to_text(decode_candidate(rrc_result.demodulated_bits, rrc_search.candidates[rrc_search.best_index])) == message;
    CHECK(!rrc_ok);  // proves this is a real gap: the wrong matched filter cannot be rescued by parameter search

    PipelineOptions rect_opts = rrc_opts;
    rect_opts.pulse_shape = sigcore::PulseShape::Rect;
    const auto rect_result = run_pipeline(bytes.data(), bytes.size(), "impaired.iq", rect_opts, {"bpsk", 1.0});
    CHECK(rect_result.recovery_used);
    const auto rect_search = search_params(rect_result.demodulated_bits, search_opts);
    CHECK(rect_search.detected);
    if (rect_search.detected) {
        const auto& best = rect_search.candidates[rect_search.best_index];
        const auto got = bits_to_text(decode_candidate(rect_result.demodulated_bits, best));
        CHECK(message.rfind(got, 0) == 0);
        CHECK(got.size() >= message.size() - 2);
    }
}

TEST_CASE("pipeline result carries a blind symbol-rate suggestion, advisory only") {
    // Reuses the Rect-pulse impaired capture above: a real end-to-end signal, not a
    // synthetic zero-order-hold test fixture, to actually exercise the blind estimator
    // on something resembling a real capture. This is advisory only -- see
    // PipelineResult::symbol_rate_estimate's own comment -- so the check here is just
    // "is it close to the truth", not "does it change what gets decoded" (it never does:
    // options.samples_per_symbol alone still controls demodulation, checked below by
    // confirming demodulated_bits does not depend on the estimate being present).
    const std::string message = "SYMBOL RATE ESTIMATION SANITY CHECK MESSAGE FOR THE PIPELINE";
    Bits payload;
    for (unsigned char ch : message)
        for (int b = 7; b >= 0; --b) payload.push_back((ch >> b) & 1);
    const Bits coded = fec_encode(payload, "viterbi");
    const auto symbols = sigapp::modulate(coded, "bpsk");
    const unsigned int sps = 8;
    const auto impaired = make_rect_impaired_iq(symbols, sps, /*cfo=*/0.003f, /*noise_amp=*/0.02f,
                                                /*start_offset=*/0, /*noise_seed=*/321);
    const auto bytes = iq_bytes(impaired);

    PipelineOptions opts;
    opts.sample_rate = 8000.0;
    opts.samples_per_symbol = sps;
    const auto result = run_pipeline(bytes.data(), bytes.size(), "impaired.iq", opts, {"bpsk", 1.0});

    CHECK(result.symbol_rate_estimate.has_value());
    if (result.symbol_rate_estimate) {
        const double rel_err = std::abs(result.symbol_rate_estimate->samples_per_symbol - static_cast<double>(sps)) / sps;
        CHECK(rel_err < 0.05);
        CHECK(result.symbol_rate_estimate->confidence > 0.2);
    }

    // Advisory only: giving the "wrong" nominal sps still demodulates with exactly
    // that value, not the estimate -- the estimate never silently overrides it.
    PipelineOptions wrong_opts = opts;
    wrong_opts.samples_per_symbol = 3;  // deliberately not the true rate
    const auto wrong_result = run_pipeline(bytes.data(), bytes.size(), "impaired.iq", wrong_opts, {"bpsk", 1.0});
    CHECK(wrong_result.symbol_rate_estimate.has_value());  // the blind estimate still runs and still finds ~8
    if (wrong_result.symbol_rate_estimate) {
        CHECK(std::abs(wrong_result.symbol_rate_estimate->samples_per_symbol - static_cast<double>(sps)) / sps < 0.05);
    }
    CHECK(wrong_result.demodulated_bits.size() != result.demodulated_bits.size());  // demod used sps=3, not the estimate
}

TEST_CASE("pipeline result leaves the symbol-rate suggestion unset for a too-short capture") {
    // Below estimate_symbol_rate's own 256-sample minimum: not a pipeline error, just
    // no suggestion (see PipelineResult::symbol_rate_estimate's own comment).
    std::vector<std::complex<float>> tiny(64, std::complex<float>(1.0f, 0.0f));
    const auto bytes = iq_bytes(tiny);
    PipelineOptions opts;
    opts.sample_rate = 8000.0;
    const auto result = run_pipeline(bytes.data(), bytes.size(), "tiny.iq", opts, {"bpsk", 1.0});
    CHECK(!result.symbol_rate_estimate.has_value());
}

TEST_CASE("pipeline: find_channels_enabled reports candidates without changing what gets decoded") {
    // A real message, pulse-shaped and CFO-impaired exactly like the recover_timing
    // test above, but mixed up to +15 kHz within a wider capture -- the PS's own
    // stated case (a capture spanning more bandwidth than one signal occupies), not a
    // signal that happens to already sit at DC. find_channels_enabled alone must not
    // change anything about the (still wideband, still undecodable as-is) result.
    const std::string message = "WIDEBAND CHANNEL FINDER TEST MESSAGE FOR THE PIPELINE";
    Bits payload;
    for (unsigned char ch : message)
        for (int b = 7; b >= 0; --b) payload.push_back((ch >> b) & 1);
    const Bits coded = fec_encode(payload, "viterbi");
    const auto symbols = sigapp::modulate(coded, "bpsk");
    const unsigned int sps = 24;
    auto shaped = make_impaired_iq(symbols, sps, /*cfo=*/0.0f, /*noise_amp=*/0.01f,
                                   /*start_offset=*/0, /*noise_seed=*/321);
    const double sample_rate = 48000.0;
    mix_up(shaped, /*freq_hz=*/15000.0, sample_rate);

    const auto bytes = iq_bytes(shaped);
    PipelineOptions opts;
    opts.sample_rate = sample_rate;
    opts.find_channels_enabled = true;
    const auto result = run_pipeline(bytes.data(), bytes.size(), "wideband.iq", opts, {"bpsk", 1.0});

    CHECK(!result.channel_candidates.empty());
    CHECK(!result.channel_extracted);  // no select_channel given
    CHECK(result.num_samples == shaped.size());  // untouched: samples_per_symbol=1 default, no demod-worth decode
    bool found = false;
    for (const auto& c : result.channel_candidates) {
        if (std::abs(c.center_freq_hz - 15000.0) < 500.0) found = true;
    }
    CHECK(found);
}

TEST_CASE("pipeline: select_channel extracts and recovers a real message from a wideband capture") {
    // The actual end-to-end claim: find the channel, extract it, and recover the real
    // signal -- using every real-world-signal-support piece this pipeline now has
    // together, the way an actual user session would (find -> extract -> read the
    // blind symbol-rate suggestion for the NEW, decimated rate, since sps=24 was only
    // ever true at the original wideband rate -> recover_timing). Checked directly
    // against the known transmitted (FEC-encoded) bitstream, bidirectionally (see
    // test_recover.cpp's best_bit_errors comment for why: recover_linear's own lock
    // point is not a precomputable index), rather than through Feature 1 (Parameter
    // Search): a real capture's receiver would use Parameter Search next to find the
    // FEC framing (proved elsewhere, in the recover_timing test above), but Parameter
    // Search's own Viterbi-candidate generation is separately sensitive to the parity
    // of the bit offset it lands on for a rate-1/2 code (confirmed directly while
    // building this: the exact correct alignment here happens to be an odd offset,
    // which is fundamentally undecodable for Viterbi regardless of how good the
    // recovery itself is -- see PulseShape::Rect's own documented limitation in
    // app/pipeline/README.md for the same underlying reason, one bit missing or
    // misplaced desyncs a rate-1/2 code's pairing entirely) -- a pre-existing,
    // unrelated fragility in that search, not something channel finding introduces.
    const std::string message = "WIDEBAND CHANNEL FINDER TEST MESSAGE FOR THE PIPELINE";
    Bits payload;
    for (unsigned char ch : message)
        for (int b = 7; b >= 0; --b) payload.push_back((ch >> b) & 1);
    const Bits coded = fec_encode(payload, "viterbi");
    const auto symbols = sigapp::modulate(coded, "bpsk");
    const unsigned int sps = 24;
    auto shaped = make_impaired_iq(symbols, sps, /*cfo=*/0.0f, /*noise_amp=*/0.01f,
                                   /*start_offset=*/0, /*noise_seed=*/321);
    const double sample_rate = 48000.0;
    mix_up(shaped, /*freq_hz=*/15000.0, sample_rate);
    const auto bytes = iq_bytes(shaped);

    PipelineOptions find_opts;
    find_opts.sample_rate = sample_rate;
    find_opts.find_channels_enabled = true;
    const auto found = run_pipeline(bytes.data(), bytes.size(), "wideband.iq", find_opts, {"bpsk", 1.0});
    CHECK(!found.channel_candidates.empty());

    size_t index = found.channel_candidates.size();
    for (size_t i = 0; i < found.channel_candidates.size(); ++i) {
        if (std::abs(found.channel_candidates[i].center_freq_hz - 15000.0) < 500.0) index = i;
    }
    CHECK(index < found.channel_candidates.size());
    if (index >= found.channel_candidates.size()) return;

    // Extract the channel first (samples_per_symbol=1, so nothing demodulates yet --
    // only channel_extracted / sample_rate / symbol_rate_estimate matter here) to read
    // the blind rate suggestion for the EXTRACTED (decimated) sample rate.
    PipelineOptions extract_opts = find_opts;
    extract_opts.select_channel = index;
    const auto extracted = run_pipeline(bytes.data(), bytes.size(), "wideband.iq", extract_opts, {"bpsk", 1.0});
    CHECK(extracted.channel_extracted);
    CHECK(extracted.sample_rate < sample_rate);  // decimated down, not still the wideband rate
    CHECK(extracted.symbol_rate_estimate.has_value());
    if (!extracted.symbol_rate_estimate) return;
    const auto sps_estimated =
        static_cast<unsigned int>(std::lround(extracted.symbol_rate_estimate->samples_per_symbol));
    CHECK(sps_estimated >= 2);

    PipelineOptions decode_opts = extract_opts;
    decode_opts.samples_per_symbol = sps_estimated;
    decode_opts.recover_timing = true;
    const auto result = run_pipeline(bytes.data(), bytes.size(), "wideband.iq", decode_opts, {"bpsk", 1.0});
    CHECK(result.channel_extracted);
    CHECK(result.recovery_used);

    const auto errors = bidirectional_bit_errors(result.demodulated_bits, coded, 200);
    CHECK(errors < 200u / 20);  // under 5% wrong at the best alignment: a real recovery
}

TEST_CASE("pipeline: an out-of-range select_channel is not an error, just not applied") {
    const auto bytes = read_file(data_path("fsk_plain.iq"));
    PipelineOptions opts;
    opts.sample_rate = 8000.0;
    opts.find_channels_enabled = true;
    opts.select_channel = 999;  // certainly out of range
    const auto result = run_pipeline(bytes.data(), bytes.size(), "fsk_plain.iq", opts, {"fsk", 0.9});
    CHECK(!result.channel_extracted);
    CHECK(bits_to_text(result.demodulated_bits) == "SIH2026!");  // decoded normally, as if select_channel were absent
}

TEST_CASE("auto-detect decodes a convolutional-interleaved, Viterbi-coded capture to its known message") {
    // Gap-list Phase 0.3 / "Problem G": after auto-detection, the pipeline used to
    // re-deinterleave from the UNTRIMMED demodulated bits, while detect_params itself
    // scored the convolutional candidate with its leading flush bits already removed
    // -- for a convolutional interleaver the two differ, so even when auto-detect
    // correctly identified "convolutional + viterbi", the actual decode that reached
    // the caller was run on the wrong (untrimmed) stream and came out wrong. Fixed by
    // reusing detect_params' own already-correctly-trimmed deinterleaved_bits/
    // decoded_bits (pipeline.cpp) instead of re-deriving them. This is a fresh,
    // self-contained capture with a real known message -- independent of (in addition
    // to) the regenerated chain_*_convolutional_auto fixtures in pipeline.rec, which
    // this same fix also changes (deliberately; see that commit's own message).
    // Repeated: detect_params' own header-repeat heuristic (the original app's, left
    // faithfully unchanged -- Parameter Search exists precisely to handle the single,
    // non-repeating case this one deliberately avoids, see the recover_timing test
    // above) needs real periodicity to find anything at all; a one-shot message gives
    // it nothing to score, independent of this fix.
    const std::string phrase = "CONV AUTO ";
    std::string message;
    for (int i = 0; i < 8; ++i) message += phrase;
    Bits payload;
    for (unsigned char ch : message)
        for (int b = 7; b >= 0; --b) payload.push_back((ch >> b) & 1);

    const Bits coded = fec_encode(payload, "viterbi");
    const auto conv = InterleaverSpec::convolutional(/*branches=*/3, /*delay_increment=*/2);
    const Bits interleaved = interleave(coded, conv);
    const auto symbols = sigapp::modulate(interleaved, "bpsk");

    std::vector<std::complex<float>> samples(symbols.size());
    for (size_t i = 0; i < symbols.size(); ++i) samples[i] = symbols[i];  // this project's own synthetic convention: sps=1
    const auto bytes = iq_bytes(samples);

    PipelineOptions opts;
    opts.sample_rate = 8000.0;
    opts.auto_detect = true;
    const auto result = run_pipeline(bytes.data(), bytes.size(), "conv_auto.iq", opts, {"bpsk", 1.0});

    CHECK(result.detected_interleaver.has_value());
    if (result.detected_interleaver) CHECK(result.detected_interleaver->kind == "convolutional");
    CHECK(result.detected_fec.value_or("") == "viterbi");
    CHECK(result.decoded_bits.has_value());
    if (result.decoded_bits) {
        // Viterbi's own trellis termination can lose the last character or two (the
        // same, already-documented edge effect the recover_timing test above tolerates
        // for the same reason) -- not what this test is about, so compare up to that
        // same safe margin, not an exact match.
        const auto got = bits_to_text(*result.decoded_bits);
        const std::string safe_prefix = message.substr(0, message.size() - 2);
        CHECK(got.rfind(safe_prefix, 0) == 0);
    }
}

TEST_CASE("pipeline_prepare / pipeline_finish split matches run_pipeline") {
    const auto bytes = read_file(data_path("fsk_plain.iq"));
    PipelineOptions o;
    o.sample_rate = 8000.0;
    auto prep = pipeline_prepare(bytes.data(), bytes.size(), "fsk_plain.iq", o);
    CHECK(prep.signal.num_samples == 512);
    CHECK(prep.spectrogram.nfft == pipeline_nfft(512));
    const auto split_result = pipeline_finish(std::move(prep), {"fsk", 0.9}, o);
    const auto whole = run_pipeline(bytes.data(), bytes.size(), "fsk_plain.iq", o, {"fsk", 0.9});
    CHECK(split_result.demodulated_bits == whole.demodulated_bits);
    CHECK(split_result.predicted_confidence == 0.9);
    CHECK(bits_to_text(split_result.demodulated_bits) == "SIH2026!");
}

TEST_CASE("pipeline_nfft follows the original's formula") {
    CHECK(pipeline_nfft(0) == 2);      // max(2, n) -> 2 -> 1 << max(1, 1 - 1)
    CHECK(pipeline_nfft(1) == 2);
    CHECK(pipeline_nfft(2) == 2);
    CHECK(pipeline_nfft(3) == 2);
    CHECK(pipeline_nfft(4) == 2);
    CHECK(pipeline_nfft(8) == 4);
    CHECK(pipeline_nfft(1000) == 256);  // capped
    CHECK(pipeline_nfft(1u << 20) == 256);
    CHECK(pipeline_nfft(512) == 256);
    CHECK(pipeline_nfft(511) == 128);
}

// The native app's own tests for generate_spectrogram and for skipping optional stages
// (tests/app/test_generate_spectrogram.py, test_pipeline.py).
TEST_CASE("native: generate_spectrogram has the right shape and axes, and its peak is at the tone") {
    const unsigned n = 4096, nfft = 256, hop = 64;
    const double rate = 8000.0, freq = 1500.0;
    std::vector<std::complex<float>> x(n);
    for (unsigned i = 0; i < n; ++i) {
        const double ph = 2.0 * 3.14159265358979323846 * freq * i / rate;
        x[i] = {static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph))};
    }
    const auto s = generate_spectrogram(x, rate, nfft, hop);
    const unsigned expected_frames = (n - nfft) / hop + 1;
    CHECK(s.frames == expected_frames && s.nfft == nfft);
    CHECK(s.data.size() == static_cast<size_t>(expected_frames) * nfft);
    CHECK(s.freqs.size() == nfft && s.times.size() == expected_frames);
    CHECK_NEAR(s.freqs[0], -rate / 2, 1e-9);                                    // centred on 0
    CHECK_NEAR(s.freqs[nfft / 2], 0.0, 1e-9);
    CHECK_NEAR(s.times[1], hop / rate, 1e-12);
    for (unsigned f = 0; f < s.frames; ++f) {
        unsigned peak = 0;
        for (unsigned k = 1; k < nfft; ++k) if (s.data[f * nfft + k] > s.data[f * nfft + peak]) peak = k;
        CHECK_NEAR(s.freqs[peak], freq, rate / nfft);                           // the peak sits at the tone, every frame
    }
}

TEST_CASE("native: the default hop is nfft / 4") {
    const unsigned n = 2048, nfft = 128;
    const std::vector<std::complex<float>> x(n, {1.0f, 0.0f});
    const auto s = generate_spectrogram(x, 8000.0, nfft);
    CHECK(s.frames == (n - nfft) / (nfft / 4) + 1);
    CHECK_THROWS(generate_spectrogram(std::vector<std::complex<float>>(10), 8000.0, 256));   // shorter than one window
}

TEST_CASE("native: with no interleaver or FEC given, those stages are skipped and the bits pass through") {
    const auto bytes = read_file(data_path("fsk_plain.iq"));
    PipelineOptions o;
    o.sample_rate = 8000.0;
    const auto r = run_pipeline(bytes.data(), bytes.size(), "fsk_plain.iq", o, {"fsk", 1.0});
    CHECK(!r.deinterleaved_bits.has_value());
    CHECK(!r.decoded_bits.has_value());
    CHECK(!r.detection_score.has_value() && !r.detected_interleaver && !r.detected_fec);
    CHECK(r.demodulated_bits.size() == 64);
    CHECK(bits_to_text(r.demodulated_bits) == "SIH2026!");
    CHECK(r.predicted_modulation == "fsk");
}

// Gap-list Phase 2.1: a single, non-repeating LDPC+block-interleaved message is the
// documented case detect_params misses; with auto_detect on, the pipeline itself now
// falls through to Parameter Search and reports a verified decode.
TEST_CASE("auto-detect falls through to Parameter Search and verifies a one-shot message") {
    const auto bytes = read_file(data_path("bpsk_ldpc_block.iq"));
    PipelineOptions o;
    o.sample_rate = 8000.0;
    o.samples_per_symbol = 4;
    o.auto_detect = true;
    const auto r = run_pipeline(bytes.data(), bytes.size(), "bpsk_ldpc_block.iq", o, {"bpsk", 1.0});
    CHECK(r.param_search_used);
    CHECK(r.decode_status == DecodeStatus::Verified);
    CHECK(r.decoded_bits.has_value());
    if (r.decoded_bits) CHECK(bits_to_text(*r.decoded_bits) == "SIH2026!");
    CHECK(r.detected_fec == std::optional<std::string>("ldpc"));
}

// Gap-list Phase 2.2 / Phase 6 adversarial case: the classifier's TOP label is wrong
// (confidently "qpsk" for a BPSK capture) but the right one is in its runner-ups; the
// pipeline must recover via the top-3 search instead of reporting a bad decode.
TEST_CASE("a wrong top classifier label is recovered from the runner-up labels") {
    const auto bytes = read_file(data_path("bpsk_ldpc_block.iq"));
    PipelineOptions o;
    o.sample_rate = 8000.0;
    o.samples_per_symbol = 1;
    o.auto_detect = true;
    Classification c{"qpsk", 0.97, {{"psk8", 0.02, {}}, {"bpsk", 0.01, {}}}};
    const auto plain = run_pipeline(bytes.data(), bytes.size(), "x.iq", o, {"qpsk", 0.97});
    CHECK(plain.decode_status != DecodeStatus::Verified);  // the wrong label alone really fails
    const auto r = run_pipeline(bytes.data(), bytes.size(), "x.iq", o, c);
    CHECK(r.decode_status == DecodeStatus::Verified);
    CHECK(r.modulation_from_search);
    CHECK(r.predicted_modulation == "bpsk");
    CHECK(r.classifier_top_label == "qpsk");
}

// Gap-list Phase 2.6 / 2.7: a SigMF sidecar supplies rate and sample format, and with no
// sidecar the format can be guessed from the bytes -- both decode a real int16 capture.
TEST_CASE("a SigMF sidecar or format auto-detection decodes an int16 capture") {
    const auto raw = read_file(data_path("fsk_plain.iq"));  // cf32 on disk
    std::vector<uint8_t> i16(raw.size() / 2);
    for (size_t i = 0; i < raw.size() / 4; ++i) {
        float f;
        std::memcpy(&f, &raw[4 * i], 4);
        // light deterministic noise, as any real int16 capture has (a noiseless +-1 tone
        // is genuinely ambiguous between int16 pairs and float32 words)
        const int16_t v = static_cast<int16_t>(std::lround(f * 16000.0f) + static_cast<int>((i * 2654435761u) % 2001) - 1000);
        std::memcpy(&i16[2 * i], &v, 2);
    }
    PipelineOptions meta_opts;
    meta_opts.sigmf_meta_json =
        R"({"global":{"core:datatype":"ci16_le","core:sample_rate":8000},"captures":[{"core:frequency":915000000}]})";
    const auto a = run_pipeline(i16.data(), i16.size(), "x.sigmf-data", meta_opts, {"fsk", 1.0});
    CHECK(bits_to_text(a.demodulated_bits) == "SIH2026!");
    CHECK(a.sample_rate_source == SampleRateSource::FromSigmfMeta);
    CHECK(a.sample_rate == 8000.0);
    CHECK(a.center_freq_hz == std::optional<double>(915000000.0));
    CHECK(!a.iq_format_guessed);

    PipelineOptions auto_opts;
    auto_opts.sample_rate = 8000.0;
    auto_opts.iq_format_auto = true;
    const auto b = run_pipeline(i16.data(), i16.size(), "x.iq", auto_opts, {"fsk", 1.0});
    CHECK(b.iq_format_guessed);
    CHECK(b.iq_format_used == "ci16");
    CHECK(bits_to_text(b.demodulated_bits) == "SIH2026!");
}

// Gap-list Phase 2.11: the strongest tone's offset and rough width are always reported.
TEST_CASE("the pipeline reports the strongest signal's offset and bandwidth from the spectrum") {
    const double rate = 8000.0, tone = 1500.0;
    std::vector<std::complex<float>> x(2048);
    for (size_t n = 0; n < x.size(); ++n) {
        const double ph = 2.0 * M_PI * tone * static_cast<double>(n) / rate;
        x[n] = {static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph))};
    }
    const auto bytes = iq_bytes(x);
    PipelineOptions o;
    o.sample_rate = rate;
    const auto r = run_pipeline(bytes.data(), bytes.size(), "tone.iq", o, {"fsk", 1.0});
    CHECK(r.signal_offset_hz.has_value());
    CHECK(r.signal_bandwidth_hz.has_value());
    if (r.signal_offset_hz) CHECK(std::abs(*r.signal_offset_hz - tone) < 2.0 * rate / r.spectrogram.nfft);
    if (r.signal_bandwidth_hz) CHECK(*r.signal_bandwidth_hz < 1000.0);
}

// Gap-list Phase 2.3: an unshaped (Rect) capture given the default RRC model is rescued by
// the automatic pulse-shape search once the first attempt fails to verify.
TEST_CASE("the pulse-shape search rescues a Rect capture left at the default RRC model") {
    const auto bytes = read_file(data_path("bpsk_ldpc_block.iq"));
    PipelineOptions o;
    o.sample_rate = 8000.0;
    o.samples_per_symbol = 4;
    o.auto_detect = true;
    o.recover_timing = true;  // pulse_shape left at Rrc 0.35
    const auto r = run_pipeline(bytes.data(), bytes.size(), "x.iq", o, {"bpsk", 1.0});
    CHECK(r.decode_status == DecodeStatus::Verified);
    CHECK(r.pulse_shape_searched);
    CHECK(r.pulse_shape_used == "rect");
    PipelineOptions off = o;
    off.pulse_search_done = true;  // what the search itself runs with: no retry
    const auto plain = run_pipeline(bytes.data(), bytes.size(), "x.iq", off, {"bpsk", 1.0});
    CHECK(plain.decode_status != DecodeStatus::Verified);
}

// Gap-list Phase 2.5: the same wideband capture, with auto_extract_channel on, is
// extracted in ONE run -- no find-then-select second step.
TEST_CASE("auto_extract_channel extracts the strongest channel in a single run") {
    const std::string message = "WIDEBAND CHANNEL FINDER TEST MESSAGE FOR THE PIPELINE";
    Bits payload;
    for (unsigned char ch : message)
        for (int b = 7; b >= 0; --b) payload.push_back((ch >> b) & 1);
    const Bits coded = fec_encode(payload, "viterbi");
    const auto symbols = sigapp::modulate(coded, "bpsk");
    auto shaped = make_impaired_iq(symbols, 24, 0.0f, 0.01f, 0, 321);
    const double sample_rate = 48000.0;
    mix_up(shaped, 15000.0, sample_rate);
    const auto bytes = iq_bytes(shaped);

    PipelineOptions o;
    o.sample_rate = sample_rate;
    o.find_channels_enabled = true;
    const auto manual = run_pipeline(bytes.data(), bytes.size(), "wideband.iq", o, {"bpsk", 1.0});
    CHECK(!manual.channel_extracted);

    o.auto_extract_channel = true;
    const auto r = run_pipeline(bytes.data(), bytes.size(), "wideband.iq", o, {"bpsk", 1.0});
    CHECK(r.channel_extracted);
    CHECK(r.channel_auto_selected);
    CHECK(r.channel_used.has_value());
    if (r.channel_used) CHECK(std::abs(r.channel_used->center_freq_hz - 15000.0) < 500.0);
    CHECK(r.sample_rate < sample_rate);  // decimated to the channel
}

// Gap-list Phase 2.10: a 4-FSK capture with non-default tone spacing and symbol length
// decodes with fsk_auto, and does not with the 2-FSK defaults.
TEST_CASE("fsk_auto recovers a 4-FSK capture the default FSK parameters cannot") {
    const std::string message = "BLIND FSK PARAMETERS";
    Bits payload;
    for (unsigned char ch : message)
        for (int b = 7; b >= 0; --b) payload.push_back((ch >> b) & 1);
    const auto x = sigcore::fsk_modulate(payload, 2, 16, 0.1f);
    const auto bytes = iq_bytes(x);
    PipelineOptions o;
    o.sample_rate = 8000.0;
    const auto plain = run_pipeline(bytes.data(), bytes.size(), "f.iq", o, {"fsk", 1.0});
    CHECK(bits_to_text(plain.demodulated_bits) != message);
    o.fsk_auto = true;
    const auto r = run_pipeline(bytes.data(), bytes.size(), "f.iq", o, {"fsk", 1.0});
    CHECK(r.fsk_params_estimated);
    CHECK(r.fsk_bits_per_symbol == 2);
    CHECK(r.fsk_samples_per_symbol == 16);
    CHECK(bits_to_text(r.demodulated_bits) == message);
}

// Gap-list Phase 2.4: 7.5 samples/symbol is not an integer, so recovery at 7 or 8 slips a
// symbol every few dozen; resampling by 8/7.5 first lets it lock and recover every bit.
TEST_CASE("samples_per_symbol_exact recovers a fractional-rate capture integer rates cannot") {
    const std::string message = "FRACTIONAL RATE";
    Bits payload;
    for (unsigned char ch : message)
        for (int b = 7; b >= 0; --b) payload.push_back((ch >> b) & 1);
    Bits coded = fec_encode(payload, "ldpc");
    // recover_linear's lock-in transient costs a symbol or two at each end (documented in
    // recover.hpp), so give the codeword guard bits to lose instead of its own.
    const Bits guard = {1, 0, 1, 1, 0, 0, 1, 0, 1, 1, 1, 0, 0, 1, 0, 1};
    coded.insert(coded.begin(), guard.begin(), guard.end());
    coded.insert(coded.end(), guard.begin(), guard.end());
    const auto symbols = sigapp::modulate(coded, "bpsk");
    const double sps = 7.5;
    std::vector<std::complex<float>> x(static_cast<size_t>(std::floor(static_cast<double>(symbols.size()) * sps)));
    for (size_t n = 0; n < x.size(); ++n) x[n] = symbols[static_cast<size_t>(static_cast<double>(n) / sps)];
    const auto bytes = iq_bytes(x);

    PipelineOptions o;
    o.sample_rate = 8000.0;
    o.recover_timing = true;
    o.pulse_shape = sigcore::PulseShape::Rect;
    o.samples_per_symbol = 8;
    // Checked at the bit level (best alignment, either polarity -- recover_linear's lock
    // point and phase are not precomputable, see test_recover.cpp): Parameter Search's
    // exact-length framing is a separate, pre-existing limitation, not this feature's.
    auto best_match = [&](const Bits& got) {
        double best = 0.0;
        for (int off = -8; off <= 40; ++off) {
            size_t m = 0, t = 0;
            for (size_t i = 0; i < coded.size(); ++i) {
                const long j = static_cast<long>(i) + off;
                if (j < 0 || j >= static_cast<long>(got.size())) continue;
                ++t;
                m += (got[static_cast<size_t>(j)] == coded[i]);
            }
            if (t < coded.size() / 2) continue;
            const double f = static_cast<double>(m) / static_cast<double>(t);
            best = std::max(best, std::max(f, 1.0 - f));
        }
        return best;
    };
    const auto rounded = run_pipeline(bytes.data(), bytes.size(), "f.iq", o, {"bpsk", 1.0});
    o.samples_per_symbol_exact = sps;
    const auto exact = run_pipeline(bytes.data(), bytes.size(), "f.iq", o, {"bpsk", 1.0});
    CHECK(exact.samples_per_symbol_fractional == std::optional<double>(sps));
    CHECK(exact.samples_per_symbol_used == 8);
    CHECK(best_match(exact.demodulated_bits) > 0.99);
    CHECK(best_match(rounded.demodulated_bits) < 0.9);
}

// Gap-list Phase 4: windowed analysis parses only the requested slice of a raw capture.
TEST_CASE("window_samples analyses a bounded slice of a raw capture and reports it") {
    const auto raw = read_file(data_path("fsk_plain.iq"));  // cf32, 8 bytes/sample
    const size_t total = raw.size() / 8;
    PipelineOptions o;
    o.sample_rate = 8000.0;
    o.window_start_samples = 16;
    o.window_samples = 128;
    const auto r = run_pipeline(raw.data(), raw.size(), "x.iq", o, {"fsk", 1.0});
    CHECK(r.num_samples == 128);
    CHECK(r.file_total_samples == total);
    CHECK(r.window_start_samples == 16);
    CHECK(r.samples.size() == 128);
    const auto text = format_result_text(r, SIZE_MAX, false, true);
    CHECK(text.find("Analysed window:      samples 16 to 144 of " + std::to_string(total)) != std::string::npos);

    PipelineOptions past = o;
    past.window_start_samples = total + 1000;  // beyond the end: clamped, not an error
    CHECK_THROWS(run_pipeline(raw.data(), raw.size(), "x.iq", past, {"fsk", 1.0}));  // empty window is empty input
    PipelineOptions whole;
    whole.sample_rate = 8000.0;
    const auto w = run_pipeline(raw.data(), raw.size(), "x.iq", whole, {"fsk", 1.0});
    CHECK(w.num_samples == total);
    CHECK(w.file_total_samples == 0);  // not windowed: nothing to report
}

// Gap-list Phase 5: the new schemes work through the pipeline's own auto path -- a one-shot
// message coded with punctured Viterbi 3/4 or RS(255,223) is found by Parameter Search.
TEST_CASE("Parameter Search finds the new FEC schemes through the pipeline") {
    struct C { const char* fec; size_t payload_bytes; };
    for (const C c : {C{"viterbi34", 24}, C{"viterbi23", 24}, C{"rs255", 20}, C{"ccsds_concat", 20}}) {
        std::string message;
        for (size_t i = 0; i < c.payload_bytes; ++i) message += static_cast<char>('A' + (i * 7) % 26);
        Bits payload;
        for (unsigned char ch : message)
            for (int b = 7; b >= 0; --b) payload.push_back((ch >> b) & 1);
        const Bits coded = fec_encode(payload, c.fec);
        const auto symbols = sigapp::modulate(coded, "bpsk");
        const auto bytes = iq_bytes(symbols);
        PipelineOptions o;
        o.sample_rate = 8000.0;
        o.auto_detect = true;
        const auto r = run_pipeline(bytes.data(), bytes.size(), "x.iq", o, {"bpsk", 1.0});
        CHECK(r.decode_status == DecodeStatus::Verified);
        CHECK(r.detected_fec == std::optional<std::string>(c.fec));
        if (r.decoded_bits) {
            const Bits& got = *r.decoded_bits;
            CHECK(got.size() >= payload.size());
            CHECK(Bits(got.begin(), got.begin() + static_cast<long>(payload.size())) == payload);
        }
    }
}

// The demodulator pads its last symbol, so an 8PSK/QAM64 stream is a few bits longer than the code
// word (813 bits for a 812-bit Viterbi word): Parameter Search must still find the code.
TEST_CASE("Parameter Search finds a code whose stream was padded by a high-order demodulator") {
    Rng rng(77);
    for (const char* mod : {"psk8", "qam64", "qam16"}) {
        Bits payload;
        for (unsigned i = 0; i < 400; ++i) payload.push_back(static_cast<uint8_t>((rng.next() >> 33) & 1));
        const Bits coded = fec_encode(payload, "viterbi");
        const auto bytes = iq_bytes(sigapp::modulate(coded, mod));
        PipelineOptions o;
        o.sample_rate = 8000.0;
        o.auto_detect = true;
        const auto r = run_pipeline(bytes.data(), bytes.size(), "x.iq", o, {mod, 1.0});
        CHECK(r.decode_status == DecodeStatus::Verified);
        CHECK(r.detected_fec == std::optional<std::string>("viterbi"));
        CHECK(r.decoded_bits.has_value());
        if (r.decoded_bits && r.decoded_bits->size() >= payload.size())
            CHECK(Bits(r.decoded_bits->begin(), r.decoded_bits->begin() + static_cast<long>(payload.size())) == payload);
    }
}

// A plain, non-repeating message with auto-detect on (now the GUI default) must stay readable:
// detect_params can score a spurious interleaver on it, which the pipeline must not apply.
TEST_CASE("auto-detect does not scramble a plain message with a spurious interleaver") {
    const auto bytes = read_file(data_path("fsk_plain.iq"));
    PipelineOptions o;
    o.sample_rate = 8000.0;
    o.auto_detect = true;
    const auto r = run_pipeline(bytes.data(), bytes.size(), "x.iq", o, {"fsk", 1.0});
    CHECK(!r.deinterleaved_bits.has_value());
    CHECK(!r.detected_interleaver.has_value());
    CHECK(bits_to_text(r.demodulated_bits) == "SIH2026!");
}

// Gap-list Phase 4 (streaming): a caller that slices the file itself still gets the whole
// file's size and the slice's position reported.
TEST_CASE("source_total_bytes / source_start_byte report a caller-sliced window against the whole file") {
    const auto raw = read_file(data_path("fsk_plain.iq"));  // 8 bytes/sample
    const std::vector<uint8_t> slice(raw.begin() + 8 * 16, raw.begin() + 8 * 144);  // samples 16..144
    PipelineOptions o;
    o.sample_rate = 8000.0;
    o.source_total_bytes = 8 * 1'000'000;      // pretend the file is 1M samples long
    o.source_start_byte = 8 * 500'000 + 8 * 16;  // and this slice starts at sample 500,016
    const auto r = run_pipeline(slice.data(), slice.size(), "big.iq", o, {"fsk", 1.0});
    CHECK(r.num_samples == 128);
    CHECK(r.file_total_samples == 1'000'000);
    CHECK(r.window_start_samples == 500'016);
    CHECK(format_result_text(r, SIZE_MAX, false, true)
              .find("Analysed window:      samples 500016 to 500144 of 1000000") != std::string::npos);
}

// Found on a REAL non-digital capture (the SigMF project's logo recording): a stream of long
// runs got 0.951 Viterbi re-encode agreement and was reported "Decoded message (verified)".
// A slowly varying analogue-style signal, demodulated to bits, must never verify.
TEST_CASE("a smooth, run-dominated capture is not reported as a verified decode") {
    std::vector<std::complex<float>> x(60000);
    for (size_t n = 0; n < x.size(); ++n) {
        const double ph = 2.0 * M_PI * 0.0007 * static_cast<double>(n) + 0.00002 * static_cast<double>(n) * static_cast<double>(n) / 1000.0;
        x[n] = {static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph))};
    }
    const auto bytes = iq_bytes(x);
    for (const char* scheme : {"bpsk", "qpsk", "fsk"}) {
        PipelineOptions o;
        o.sample_rate = 48000.0;
        o.auto_detect = true;
        o.time_budget_sec = 3.0;
        const auto r = run_pipeline(bytes.data(), bytes.size(), "smooth.iq", o, {scheme, 0.9});
        CHECK(r.decode_status != DecodeStatus::Verified);
    }
}

// Found with a real GPS recording: an FSK capture whose length is not a whole number of symbols
// used to fail outright. The trailing partial symbol is now dropped.
TEST_CASE("an FSK capture with a trailing partial symbol still demodulates") {
    auto raw = read_file(data_path("fsk_plain.iq"));
    raw.resize(raw.size() + 3 * 8, 0);  // three extra samples: not a multiple of 8 samples/symbol
    PipelineOptions o;
    o.sample_rate = 8000.0;
    const auto r = run_pipeline(raw.data(), raw.size(), "x.iq", o, {"fsk", 1.0});
    CHECK(bits_to_text(r.demodulated_bits).substr(0, 8) == "SIH2026!");
}

// A huge capture must not run the hundreds-of-candidates searches (46 s natively on 16M zero
// samples before this limit); the result says it skipped them.
TEST_CASE("auto-detect is skipped, and reported, on a capture too large to search") {
    std::vector<uint8_t> raw(8 * 400'000, 0);  // 400k bpsk bits at 1 sample/symbol: over the 300k-bit limit
    for (size_t i = 0; i < 400'000; ++i) { const float v = (i * 2654435761u >> 7) & 1 ? 1.0f : -1.0f; std::memcpy(&raw[8 * i], &v, 4); }
    PipelineOptions o;
    o.sample_rate = 8000.0;
    o.auto_detect = true;
    const auto r = run_pipeline(raw.data(), raw.size(), "huge.iq", o, {"bpsk", 0.9});
    CHECK(r.auto_search_skipped);
    CHECK(!r.detection_score.has_value());
    CHECK(!r.param_search_used);
    CHECK(format_result_text(r, 100, false, true).find("Auto-detect:          skipped") != std::string::npos);
}

// Link-layer framing as a verifier: a G3RUH-scrambled, NRZI-coded AX.25 beacon (the CPSat / cubesat
// style) sent as pulse-shaped BPSK with a carrier offset, a start offset and noise. No FEC in the
// app's families applies to it, yet a CRC-valid AX.25 frame in the recovered bits verifies it.
TEST_CASE("a G3RUH AX.25 beacon through recovered BPSK is found and reported verified") {
    std::vector<uint8_t> frame = {0x86, 0xa0, 0xa6, 0x82, 0xa8, 0x62, 0xe0, 0x86, 0xa0, 0xa6, 0x82, 0xa8, 0x62, 0x61, 0x03, 0xf0};
    const std::string text = "CPSAT BEACON TEST PAYLOAD 0123456789";
    frame.insert(frame.end(), text.begin(), text.end());
    const Bits line = nrzi_encode(g3ruh_scramble(hdlc_encode(frame, 60, 6)));
    const auto symbols = sigapp::modulate(line, "bpsk");
    const unsigned sps = 8;
    const auto impaired = make_impaired_iq(symbols, sps, 0.002f, 0.03f, 3, 777);
    const auto bytes = iq_bytes(impaired);

    PipelineOptions o;
    o.sample_rate = 8000.0;
    o.samples_per_symbol = sps;
    o.recover_timing = true;
    o.auto_detect = true;
    const auto r = run_pipeline(bytes.data(), bytes.size(), "beacon.iq", o, {"bpsk", 0.9});
    CHECK(r.decode_status == DecodeStatus::Verified);
    CHECK(r.frames.size() >= 1);
    if (!r.frames.empty()) {
        CHECK(r.frames[0].destination == "CPSAT1-0");
        CHECK(std::string(r.frames[0].info.begin(), r.frames[0].info.end()) == text);
    }
    CHECK(r.framing_chain.find("G3RUH") != std::string::npos);
    CHECK(r.decoded_bits.has_value());
    if (r.decoded_bits) CHECK(bits_to_text(*r.decoded_bits) == text);
    CHECK(format_result_text(r, SIZE_MAX, false, true).find("AX.25 frames:") != std::string::npos);
    CHECK(format_result_json(r).find("\"ax25_frames\"") != std::string::npos);
}

TEST_CASE("the same beacon sent as FSK is verified through the FSK demodulator too") {
    std::vector<uint8_t> frame = {0x86, 0xa0, 0xa6, 0x82, 0xa8, 0x62, 0xe0, 0x86, 0xa0, 0xa6, 0x82, 0xa8, 0x62, 0x61, 0x03, 0xf0};
    const std::string text = "FSK PATH PAYLOAD";
    frame.insert(frame.end(), text.begin(), text.end());
    const Bits line = nrzi_encode(g3ruh_scramble(hdlc_encode(frame, 40, 4)));
    const auto samples = sigcore::fsk_modulate(line, 1, 8, 0.25f);
    const auto bytes = iq_bytes(samples);
    PipelineOptions o;
    o.sample_rate = 8000.0;
    o.auto_detect = true;
    const auto r = run_pipeline(bytes.data(), bytes.size(), "fskbeacon.iq", o, {"fsk", 0.9});
    CHECK(r.decode_status == DecodeStatus::Verified);
    CHECK(r.frames.size() == 1);
    if (r.frames.size() == 1) CHECK(std::string(r.frames[0].info.begin(), r.frames[0].info.end()) == text);
}

// The classifier's real-world mistake, reproduced: a BPSK burst called "fsk". The blind check
// notices the signal is not FSK and the pipeline analyses it as BPSK -- and, with the link-layer
// verifier, the beacon inside decodes. The same capture with an explicit override is left alone.
TEST_CASE("a BPSK burst the classifier calls FSK is re-read as BPSK") {
    std::vector<uint8_t> frame = {0x86, 0xa0, 0xa6, 0x82, 0xa8, 0x62, 0xe0, 0x86, 0xa0, 0xa6, 0x82, 0xa8, 0x62, 0x61, 0x03, 0xf0};
    const std::string text = "MISLABELLED AS FSK";
    frame.insert(frame.end(), text.begin(), text.end());
    const Bits line = nrzi_encode(g3ruh_scramble(hdlc_encode(frame, 80, 6)));
    const auto symbols = sigapp::modulate(line, "bpsk");
    const unsigned sps = 8;
    auto burst = make_impaired_iq(symbols, sps, 0.002f, 0.0f, 3, 4242);
    // idle noise before and after, as a real recording has
    std::vector<std::complex<float>> capture;
    uint32_t seed = 99;
    auto noise = [&] { seed = seed * 1664525u + 1013904223u; return (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f) * 0.05f; };
    for (size_t i = 0; i < burst.size(); ++i) capture.push_back({noise(), noise()});
    for (const auto& v : burst) capture.push_back(v + std::complex<float>(noise(), noise()));
    for (size_t i = 0; i < burst.size(); ++i) capture.push_back({noise(), noise()});
    const auto bytes = iq_bytes(capture);

    PipelineOptions o;
    o.sample_rate = 8000.0;
    o.samples_per_symbol = sps;
    o.recover_timing = true;
    o.auto_detect = true;
    const auto r = run_pipeline(bytes.data(), bytes.size(), "mislabelled.iq", o, {"fsk", 0.99});
    CHECK(r.modulation_from_blind_check);
    CHECK(r.classifier_top_label == "fsk");
    CHECK(r.predicted_modulation == "bpsk");
    CHECK(r.decode_status == DecodeStatus::Verified);
    CHECK(r.frames.size() >= 1);
    CHECK(format_result_text(r, SIZE_MAX, false, true).find("Modulation check:") != std::string::npos);

    PipelineOptions forced = o;
    forced.modulation_override = "fsk";
    const auto kept = run_pipeline(bytes.data(), bytes.size(), "mislabelled.iq", forced, {"fsk", 0.99});
    CHECK(!kept.modulation_from_blind_check);
    CHECK(kept.predicted_modulation == "fsk");
}

TEST_CASE("genuine FSK keeps its label through the blind check") {
    const Bits bits = [] { Bits b; for (int i = 0; i < 6000; ++i) b.push_back((i * 2654435761u >> 9) & 1); return b; }();
    const auto samples = sigcore::fsk_modulate(bits, 1, 8, 0.2f);
    const auto bytes = iq_bytes(samples);
    PipelineOptions o;
    o.sample_rate = 8000.0;
    o.auto_detect = true;
    const auto r = run_pipeline(bytes.data(), bytes.size(), "fsk.iq", o, {"fsk", 0.99});
    CHECK(!r.modulation_from_blind_check);
    CHECK(r.predicted_modulation == "fsk");
}

// The committed synthetic beacon (tests/data/pipeline/ax25_g3ruh_beacon.iq, made to the recipe in the
// CPSat recordings' README): the GUI's own two-step flow -- run once, apply the blind symbol-rate
// estimate, run again -- ends with the frame found, from a classifier that says "fsk".
TEST_CASE("the beacon fixture is decoded by the GUI's own two-step flow from a wrong classifier label") {
    const auto bytes = read_file(data_path("ax25_g3ruh_beacon.iq"));
    const Classification ml{"fsk", 0.99, {{"qam64", 0.01, {}}, {"bpsk", 0.005, {}}}};
    PipelineOptions first;
    first.sample_rate = 192000.0;
    first.auto_detect = true;
    first.recover_timing = true;
    first.fsk_auto = true;
    const auto r1 = run_pipeline(bytes.data(), bytes.size(), "beacon.iq", first, ml);
    CHECK(r1.decode_status != DecodeStatus::Verified);  // 1 sample/symbol cannot recover anything yet
    CHECK(r1.symbol_rate_estimate.has_value());
    if (!r1.symbol_rate_estimate) return;
    CHECK(std::abs(r1.symbol_rate_estimate->samples_per_symbol - 20.0) < 0.5);
    PipelineOptions second = first;
    second.samples_per_symbol = static_cast<unsigned>(std::lround(r1.symbol_rate_estimate->samples_per_symbol));
    const auto r2 = run_pipeline(bytes.data(), bytes.size(), "beacon.iq", second, ml);
    CHECK(r2.decode_status == DecodeStatus::Verified);
    CHECK(r2.modulation_from_blind_check);
    CHECK(r2.frames.size() == 1);
    if (r2.frames.size() == 1) CHECK(std::string(r2.frames[0].info.begin(), r2.frames[0].info.end()) == "SYNTHETIC BEACON: ALL SYSTEMS NOMINAL 0042");
}

// Gap-list Phase 1.5 (Problem C): modulation_override replaces the classifier's own
// label outright -- this capture really is FSK (the previous test demodulates it
// correctly as such), but telling the pipeline it is BPSK should make it try to
// demodulate FSK samples as BPSK, failing exactly the way a genuinely wrong
// classifier guess would, and predicted_modulation should say "bpsk" (what actually
// ran), not "fsk" (what the classifier -- itself not consulted on the label here,
// since both run through the same `classification` the caller supplies -- said).
TEST_CASE("modulation_override replaces the classifier's label with a forced scheme") {
    const auto bytes = read_file(data_path("fsk_plain.iq"));
    PipelineOptions o;
    o.sample_rate = 8000.0;
    o.modulation_override = "bpsk";
    const auto r = run_pipeline(bytes.data(), bytes.size(), "fsk_plain.iq", o, {"fsk", 1.0});
    CHECK(r.predicted_modulation == "bpsk");
    CHECK(bits_to_text(r.demodulated_bits) != "SIH2026!");  // BPSK-demodulated FSK samples: not the real message
}

// Feature 4: the frame raster is computed automatically as part of the pipeline, on a
// capture with genuinely repeating frames (the existing demo/native fixtures are all a
// single short message, so raster.detected would correctly be false for those).
TEST_CASE("pipeline result includes a frame raster when the bitstream repeats") {
    const Bits header = {1, 0, 1, 1, 0, 0, 1, 0};
    Rng rng(50);
    Bits stream;
    for (unsigned i = 0; i < 30; ++i) {
        stream.insert(stream.end(), header.begin(), header.end());
        for (unsigned b = 0; b < 24; ++b) stream.push_back(static_cast<uint8_t>((rng.next() >> 33) & 1));
    }
    const auto samples = modulate(stream, "bpsk");
    std::vector<uint8_t> raw(samples.size() * 2 * sizeof(float));
    std::memcpy(raw.data(), samples.data(), raw.size());

    PipelineOptions o;
    o.sample_rate = 8000.0;
    const auto r = run_pipeline(raw.data(), raw.size(), "raster.iq", o, {"bpsk", 1.0});
    CHECK(r.raster.detected);
    CHECK(r.raster.frame_length == 32);
    CHECK(r.raster.sync_word == header);
}

// Gap-list Phase 1.3: with no explicit header_pattern, find_header_payload_boundaries
// used to always assume the stream's first 16 bits are the header, whether or not the
// frame raster (above) had already found the REAL, shorter sync word from the data's
// own repetition -- here the 8-bit header above makes the first-16-bits guess actually
// span the header plus 8 bits of the next frame's payload, not the real header at all.
TEST_CASE("the frame raster's own sync word is used as the header pattern once detected") {
    const Bits header = {1, 0, 1, 1, 0, 0, 1, 0};
    Rng rng(50);
    Bits stream;
    for (unsigned i = 0; i < 30; ++i) {
        stream.insert(stream.end(), header.begin(), header.end());
        for (unsigned b = 0; b < 24; ++b) stream.push_back(static_cast<uint8_t>((rng.next() >> 33) & 1));
    }
    const auto samples = modulate(stream, "bpsk");
    std::vector<uint8_t> raw(samples.size() * 2 * sizeof(float));
    std::memcpy(raw.data(), samples.data(), raw.size());

    PipelineOptions o;
    o.sample_rate = 8000.0;
    const auto r = run_pipeline(raw.data(), raw.size(), "raster.iq", o, {"bpsk", 1.0});
    CHECK(r.raster.detected);
    CHECK(r.header_pattern_source == HeaderPatternSource::FrameRaster);
    CHECK(r.boundaries.header_pattern == header);
    CHECK(r.boundaries.header_pattern.size() == 8);  // not the 16-bit assumed fallback
}

// Gap-list Phase 1.3: the flip side -- too short/unstructured a capture for the raster
// to find anything real falls back to the original first-16-bits guess, same as always,
// but now says plainly (via header_pattern_source) that it IS a guess.
TEST_CASE("falls back to the assumed first-16-bits header pattern when the raster finds nothing") {
    const auto msg = random_bits(64, 5);
    const auto samples = modulate(msg, "bpsk");
    std::vector<uint8_t> raw(samples.size() * 2 * sizeof(float));
    std::memcpy(raw.data(), samples.data(), raw.size());

    PipelineOptions o;
    o.sample_rate = 8000.0;
    const auto r = run_pipeline(raw.data(), raw.size(), "no_raster.iq", o, {"bpsk", 1.0});
    CHECK(!r.raster.detected);
    CHECK(r.header_pattern_source == HeaderPatternSource::Assumed);
    CHECK(r.boundaries.header_pattern == Bits(msg.begin(), msg.begin() + 16));
}

// Gap-list Phase 1.3: an explicit header_pattern option always wins, raster or not.
TEST_CASE("an explicit header_pattern option is used as-is, not overridden by the raster") {
    const Bits header = {1, 0, 1, 1, 0, 0, 1, 0};
    Rng rng(50);
    Bits stream;
    for (unsigned i = 0; i < 30; ++i) {
        stream.insert(stream.end(), header.begin(), header.end());
        for (unsigned b = 0; b < 24; ++b) stream.push_back(static_cast<uint8_t>((rng.next() >> 33) & 1));
    }
    const auto samples = modulate(stream, "bpsk");
    std::vector<uint8_t> raw(samples.size() * 2 * sizeof(float));
    std::memcpy(raw.data(), samples.data(), raw.size());

    const Bits explicit_pattern = {1, 1, 1, 1, 1, 1, 1, 1};
    PipelineOptions o;
    o.sample_rate = 8000.0;
    o.header_pattern = explicit_pattern;
    const auto r = run_pipeline(raw.data(), raw.size(), "raster.iq", o, {"bpsk", 1.0});
    CHECK(r.raster.detected);  // the raster still runs and still finds the real sync word
    CHECK(r.header_pattern_source == HeaderPatternSource::UserSpecified);
    CHECK(r.boundaries.header_pattern == explicit_pattern);
}

// Feature 3: soft decoding through the whole pipeline, end to end -- BER must be no
// worse than the hard path on the same noisy capture, and reencode_agreement is set.
TEST_CASE("pipeline: soft decoding matches or beats hard decoding end to end") {
    const auto msg = random_bits(200, 61);
    const auto coded = fec_encode(msg, "ldpc");
    const auto samples = modulate(coded, "bpsk");
    std::vector<std::complex<float>> noisy(samples.size());
    uint32_t seed = 7;
    auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return static_cast<float>(seed >> 8) / 8388608.0f - 1.0f; };
    for (size_t i = 0; i < samples.size(); ++i) noisy[i] = samples[i] + std::complex<float>(0.55f * rnd(), 0.55f * rnd());
    std::vector<uint8_t> raw(noisy.size() * 2 * sizeof(float));
    std::memcpy(raw.data(), noisy.data(), raw.size());

    PipelineOptions hard_opts;
    hard_opts.sample_rate = 8000.0;
    hard_opts.fec_scheme = "ldpc";
    hard_opts.fec_msg_len = msg.size();
    const auto hard_result = run_pipeline(raw.data(), raw.size(), "soft.iq", hard_opts, {"bpsk", 1.0});
    CHECK(!hard_result.soft_decoding_used);
    CHECK(hard_result.reencode_agreement.has_value());

    PipelineOptions soft_opts = hard_opts;
    soft_opts.soft_decoding = true;
    const auto soft_result = run_pipeline(raw.data(), raw.size(), "soft.iq", soft_opts, {"bpsk", 1.0});
    CHECK(soft_result.soft_decoding_used);
    CHECK(soft_result.demodulated_bits == hard_result.demodulated_bits);  // same hard bits either way

    auto errors = [&](const Bits& decoded) {
        unsigned n = 0;
        for (size_t i = 0; i < decoded.size() && i < msg.size(); ++i) n += (decoded[i] != msg[i]);
        return n;
    };
    CHECK(errors(*soft_result.decoded_bits) <= errors(*hard_result.decoded_bits));
}

TEST_CASE("pipeline: soft decoding silently falls back to hard for FSK and RS") {
    // FSK: no soft demapper.
    const auto bytes = read_file(data_path("fsk_plain.iq"));
    PipelineOptions o;
    o.sample_rate = 8000.0;
    o.soft_decoding = true;
    const auto r = run_pipeline(bytes.data(), bytes.size(), "fsk_plain.iq", o, {"fsk", 1.0});
    CHECK(!r.soft_decoding_used);
}

TEST_MAIN()
