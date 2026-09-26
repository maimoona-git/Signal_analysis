// Phase 6: the full pipeline (ingest -> spectrogram -> demod -> de-interleave ->
// FEC decode -> correlate, with optional blind detection) on 82 scenarios
// recorded from the ORIGINAL app/pipeline.py (tests/data/pipeline/pipeline.rec):
// the native app's two demo captures with its real classifier, every modulation,
// WAV input, noisy channels, explicit and auto-detected parameters for every
// FEC x interleaver pairing, and every way the original raises.
//
// The ML classification is an input here (the recorded label/confidence): the
// classifier itself is verified separately (Phase 5), and end to end through the
// browser in tests/bridge/pipeline.test.mjs.

#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "pipeline.hpp"
#include "../harness.hpp"
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
    CHECK(recs.size() == 82);
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

TEST_MAIN()
