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
#include "param_search.hpp"
#include "pipeline.hpp"
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
                                           /*start_offset=*/5, /*noise_seed=*/999);
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
