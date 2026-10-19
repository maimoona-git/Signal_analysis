// The binary wire format between the Qt app and the analysis Web Worker (pipeline_io.hpp):
// every field survives a round trip, malformed bytes are rejected without reading out of range,
// and a real pipeline result round-trips and still formats to the same text.

#include <cstring>
#include <fstream>
#include <iterator>

#include "pipeline.hpp"
#include "pipeline_io.hpp"
#include "result_text.hpp"
#ifndef SIG_TEST_DATA_DIR
#error "SIG_TEST_DATA_DIR must point at tests/data"
#endif
#include "../harness.hpp"
#include "../testutil.hpp"

using namespace sigapp;

namespace {

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// EVERY field set to a value that differs from its default, and from each other where the
// type allows, so a field left out of the serializer cannot hide.
PipelineOptions full_options() {
    PipelineOptions o;
    o.sample_rate = 12345.5;
    o.iq_format = sigcore::IqSampleFormat::Cu8;
    o.iq_format_auto = true;
    o.sigmf_meta_json = std::string("{\"global\":{}}");
    o.samples_per_symbol = 7;
    o.samples_per_symbol_exact = 7.25;
    o.window_start_samples = 11;
    o.window_samples = 22;
    o.source_total_bytes = 33;
    o.source_start_byte = 44;
    o.demod.bits_per_symbol = 3;
    o.demod.samples_per_symbol = 24;
    o.demod.bandwidth = 0.125f;
    o.fsk_auto = true;
    o.interleaver = InterleaverSpec::convolutional(5, 3);
    o.fec_scheme = std::string("viterbi34");
    o.fec_msg_len = 99;
    o.header_pattern = Bits{1, 0, 1, 1, 0, 0, 0, 1, 1};
    o.auto_detect = true;
    o.soft_decoding = true;
    o.recover_timing = true;
    o.pulse_shape = sigcore::PulseShape::Rect;
    o.rolloff = 0.25f;
    o.pulse_search_done = true;
    o.time_budget_sec = 3.5;
    o.find_channels_enabled = true;
    o.select_channel = 2;
    o.auto_extract_channel = true;
    o.modulation_override = std::string("qam16");
    return o;
}

PipelineResult full_result() {
    PipelineResult p;
    p.file_path = "a/b c.iq";
    p.sample_rate = 8000.25;
    p.num_samples = 5;
    p.file_format = "iq_cf32";
    p.samples = {{1.0f, -2.0f}, {0.5f, 0.25f}, {3.0f, 4.0f}, {-1.0f, -1.5f}, {0.0f, 9.0f}};
    p.spectrogram.data = {1.f, 2.f, 3.f, 4.f, 5.f, 6.f};
    p.spectrogram.frames = 2;
    p.spectrogram.nfft = 3;
    p.spectrogram.freqs = {-1.0, 0.0, 1.0};
    p.spectrogram.times = {0.0, 0.5};
    p.predicted_modulation = "qpsk";
    p.predicted_confidence = 0.625;
    p.demodulated_bits = Bits{1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 1};
    p.deinterleaved_bits = Bits{0, 1};
    p.decoded_bits = Bits{1, 1, 0, 0, 1, 0, 1, 0, 1};
    p.boundaries.header_pattern = Bits{1, 0, 1};
    p.boundaries.frame_period = 40;
    p.boundaries.header_positions = {0, 40, 80};
    p.boundaries.payloads = {Bits{1, 0}, Bits{0, 0, 1}};
    p.header_pattern_source = HeaderPatternSource::FrameRaster;
    p.detected_interleaver = InterleaverSpec::block(4, 8);
    p.detected_fec = "ldpc";
    p.detection_score = 17;
    p.raster.detected = true;
    p.raster.frame_length = 16;
    p.raster.num_frames = 3;
    p.raster.matrix = Bits{1, 0, 0, 1, 1};
    p.raster.row_shift = {0, -1, 2};
    p.raster.columns = {{0.5, 0.25, 0.125, ColumnLabel::CounterLike}, {1.0, 1.0, 0.0, ColumnLabel::Constant}};
    p.raster.sync_word = Bits{1, 1, 0};
    p.raster.header_columns = {0, 1, 2};
    p.raster.sync_positions = {5, 21};
    p.rank_analysis.detected = true;
    p.rank_analysis.width = 48;
    p.rank_analysis.rank = 23.5;
    p.rank_analysis.code_rate = 0.49;
    p.rank_analysis.windows_used = 6;
    p.rank_analysis.profile = {{2, 1.5, 0.25, 0.75}, {48, 23.5, 0.5, 24.0}};
    p.soft_decoding_used = true;
    p.reencode_agreement = 0.97;
    p.recovery_used = true;
    p.recovery_phase_ambiguity = 4;
    p.symbol_rate_estimate = sigcore::SymbolRateEstimate{7.5, 0.8};
    p.samples_per_symbol_used = 8;
    p.samples_per_symbol_fractional = 7.5;
    p.channel_candidates = {{100.0, 2000.0, 1.0}, {-300.0, 500.0, 0.25}};
    p.channel_extracted = true;
    p.channel_used = sigcore::ChannelCandidate{-300.0, 500.0, 0.25};
    p.channel_auto_selected = true;
    p.channel_index = 1;
    p.sample_rate_source = SampleRateSource::FromSigmfMeta;
    p.file_total_samples = 1'000'000;
    p.window_start_samples = 500'000;
    p.center_freq_hz = 433.92e6;
    p.iq_format_guessed = true;
    p.iq_format_used = "ci16";
    p.signal_offset_hz = -93.75;
    p.signal_bandwidth_hz = 281.25;
    p.decode_status = DecodeStatus::Verified;
    p.param_search_used = true;
    p.param_search_bit_offset = 13;
    p.param_search_confidence = 0.91;
    p.auto_search_skipped = true;
    p.modulation_from_search = true;
    p.classifier_top_label = "psk8";
    p.pulse_shape_searched = true;
    p.fsk_params_estimated = true;
    p.fsk_bits_per_symbol = 2;
    p.fsk_samples_per_symbol = 16;
    p.fsk_bandwidth = 0.1f;
    p.fsk_confidence = 0.99;
    p.pulse_shape_used = "rrc 0.25";
    return p;
}

bool same(const InterleaverSpec& a, const InterleaverSpec& b) { return a == b; }

}  // namespace

TEST_CASE("PipelineOptions: every field survives a round trip (the deadline is deliberately not carried)") {
    const PipelineOptions o = full_options();
    const auto bytes = serialize_options(o);
    const PipelineOptions r = deserialize_options(bytes.data(), bytes.size());
    CHECK(r.sample_rate == o.sample_rate);
    CHECK(r.iq_format == o.iq_format);
    CHECK(r.iq_format_auto == o.iq_format_auto);
    CHECK(r.sigmf_meta_json == o.sigmf_meta_json);
    CHECK(r.samples_per_symbol == o.samples_per_symbol);
    CHECK(r.samples_per_symbol_exact == o.samples_per_symbol_exact);
    CHECK(r.window_start_samples == o.window_start_samples);
    CHECK(r.window_samples == o.window_samples);
    CHECK(r.source_total_bytes == o.source_total_bytes);
    CHECK(r.source_start_byte == o.source_start_byte);
    CHECK(r.demod.bits_per_symbol == o.demod.bits_per_symbol);
    CHECK(r.demod.samples_per_symbol == o.demod.samples_per_symbol);
    CHECK(r.demod.bandwidth == o.demod.bandwidth);
    CHECK(r.fsk_auto == o.fsk_auto);
    CHECK(r.interleaver.has_value() && same(*r.interleaver, *o.interleaver));
    CHECK(r.fec_scheme == o.fec_scheme);
    CHECK(r.fec_msg_len == o.fec_msg_len);
    CHECK(r.header_pattern == o.header_pattern);
    CHECK(r.auto_detect == o.auto_detect);
    CHECK(r.soft_decoding == o.soft_decoding);
    CHECK(r.recover_timing == o.recover_timing);
    CHECK(r.pulse_shape == o.pulse_shape);
    CHECK(r.rolloff == o.rolloff);
    CHECK(r.pulse_search_done == o.pulse_search_done);
    CHECK(r.time_budget_sec == o.time_budget_sec);
    CHECK(r.find_channels_enabled == o.find_channels_enabled);
    CHECK(r.select_channel == o.select_channel);
    CHECK(r.auto_extract_channel == o.auto_extract_channel);
    CHECK(r.modulation_override == o.modulation_override);
    CHECK(!r.deadline.has_value());

    // and the all-default options too (every optional empty)
    const auto d = serialize_options(PipelineOptions{});
    const PipelineOptions rd = deserialize_options(d.data(), d.size());
    CHECK(!rd.sample_rate && !rd.interleaver && !rd.header_pattern && !rd.modulation_override);
    CHECK(rd.time_budget_sec == PipelineOptions{}.time_budget_sec);
}

TEST_CASE("Classification round-trips with its alternatives, and absurd nesting is refused") {
    Classification c{"fsk", 0.75, {{"bpsk", 0.125, {}}, {"qpsk", 0.0625, {}}}};
    const auto bytes = serialize_classification(c);
    const auto r = deserialize_classification(bytes.data(), bytes.size());
    CHECK(r.label == "fsk" && r.confidence == 0.75);
    CHECK(r.alternatives.size() == 2);
    CHECK(r.alternatives[1].label == "qpsk" && r.alternatives[1].confidence == 0.0625);

    Classification deep{"a", 1.0, {}};
    Classification* cur = &deep;
    for (int i = 0; i < 8; ++i) { cur->alternatives.push_back({"b", 1.0, {}}); cur = &cur->alternatives.back(); }
    CHECK_THROWS(serialize_classification(deep));
}

TEST_CASE("PipelineResult: every field survives a round trip") {
    const PipelineResult p = full_result();
    const auto bytes = serialize_result(p);
    const PipelineResult r = deserialize_result(bytes.data(), bytes.size());
    CHECK(r.file_path == p.file_path);
    CHECK(r.sample_rate == p.sample_rate);
    CHECK(r.num_samples == p.num_samples);
    CHECK(r.file_format == p.file_format);
    CHECK(r.samples == p.samples);
    CHECK(r.spectrogram.data == p.spectrogram.data);
    CHECK(r.spectrogram.frames == p.spectrogram.frames);
    CHECK(r.spectrogram.nfft == p.spectrogram.nfft);
    CHECK(r.spectrogram.freqs == p.spectrogram.freqs);
    CHECK(r.spectrogram.times == p.spectrogram.times);
    CHECK(r.predicted_modulation == p.predicted_modulation);
    CHECK(r.predicted_confidence == p.predicted_confidence);
    CHECK(r.demodulated_bits == p.demodulated_bits);
    CHECK(r.deinterleaved_bits == p.deinterleaved_bits);
    CHECK(r.decoded_bits == p.decoded_bits);
    CHECK(r.boundaries.header_pattern == p.boundaries.header_pattern);
    CHECK(r.boundaries.frame_period == p.boundaries.frame_period);
    CHECK(r.boundaries.header_positions == p.boundaries.header_positions);
    CHECK(r.boundaries.payloads == p.boundaries.payloads);
    CHECK(r.header_pattern_source == p.header_pattern_source);
    CHECK(r.detected_interleaver.has_value() && same(*r.detected_interleaver, *p.detected_interleaver));
    CHECK(r.detected_fec == p.detected_fec);
    CHECK(r.detection_score == p.detection_score);
    CHECK(r.raster.detected == p.raster.detected);
    CHECK(r.raster.frame_length == p.raster.frame_length);
    CHECK(r.raster.num_frames == p.raster.num_frames);
    CHECK(r.raster.matrix == p.raster.matrix);
    CHECK(r.raster.row_shift == p.raster.row_shift);
    CHECK(r.raster.columns.size() == 2);
    CHECK(r.raster.columns[0].ones_fraction == 0.5 && r.raster.columns[0].stability == 0.25 &&
          r.raster.columns[0].transition_rate == 0.125 && r.raster.columns[0].label == ColumnLabel::CounterLike);
    CHECK(r.raster.columns[1].label == ColumnLabel::Constant);
    CHECK(r.raster.sync_word == p.raster.sync_word);
    CHECK(r.raster.header_columns == p.raster.header_columns);
    CHECK(r.raster.sync_positions == p.raster.sync_positions);
    CHECK(r.rank_analysis.detected == p.rank_analysis.detected);
    CHECK(r.rank_analysis.width == p.rank_analysis.width);
    CHECK(r.rank_analysis.rank == p.rank_analysis.rank);
    CHECK(r.rank_analysis.code_rate == p.rank_analysis.code_rate);
    CHECK(r.rank_analysis.windows_used == p.rank_analysis.windows_used);
    CHECK(r.rank_analysis.profile.size() == 2);
    CHECK(r.rank_analysis.profile[1].width == 48 && r.rank_analysis.profile[1].rank == 23.5 &&
          r.rank_analysis.profile[1].baseline_deficiency == 0.5 && r.rank_analysis.profile[1].excess_deficiency == 24.0);
    CHECK(r.soft_decoding_used == p.soft_decoding_used);
    CHECK(r.reencode_agreement == p.reencode_agreement);
    CHECK(r.recovery_used == p.recovery_used);
    CHECK(r.recovery_phase_ambiguity == p.recovery_phase_ambiguity);
    CHECK(r.symbol_rate_estimate.has_value() && r.symbol_rate_estimate->samples_per_symbol == 7.5 &&
          r.symbol_rate_estimate->confidence == 0.8);
    CHECK(r.samples_per_symbol_used == p.samples_per_symbol_used);
    CHECK(r.samples_per_symbol_fractional == p.samples_per_symbol_fractional);
    CHECK(r.channel_candidates.size() == 2 && r.channel_candidates[1].center_freq_hz == -300.0 &&
          r.channel_candidates[1].bandwidth_hz == 500.0 && r.channel_candidates[1].confidence == 0.25);
    CHECK(r.channel_extracted == p.channel_extracted);
    CHECK(r.channel_used.has_value() && r.channel_used->center_freq_hz == -300.0);
    CHECK(r.channel_auto_selected == p.channel_auto_selected);
    CHECK(r.channel_index == p.channel_index);
    CHECK(r.sample_rate_source == p.sample_rate_source);
    CHECK(r.file_total_samples == p.file_total_samples);
    CHECK(r.window_start_samples == p.window_start_samples);
    CHECK(r.center_freq_hz == p.center_freq_hz);
    CHECK(r.iq_format_guessed == p.iq_format_guessed);
    CHECK(r.iq_format_used == p.iq_format_used);
    CHECK(r.signal_offset_hz == p.signal_offset_hz);
    CHECK(r.signal_bandwidth_hz == p.signal_bandwidth_hz);
    CHECK(r.decode_status == p.decode_status);
    CHECK(r.param_search_used == p.param_search_used);
    CHECK(r.param_search_bit_offset == p.param_search_bit_offset);
    CHECK(r.param_search_confidence == p.param_search_confidence);
    CHECK(r.auto_search_skipped == p.auto_search_skipped);
    CHECK(r.modulation_from_search == p.modulation_from_search);
    CHECK(r.classifier_top_label == p.classifier_top_label);
    CHECK(r.pulse_shape_searched == p.pulse_shape_searched);
    CHECK(r.fsk_params_estimated == p.fsk_params_estimated);
    CHECK(r.fsk_bits_per_symbol == p.fsk_bits_per_symbol);
    CHECK(r.fsk_samples_per_symbol == p.fsk_samples_per_symbol);
    CHECK(r.fsk_bandwidth == p.fsk_bandwidth);
    CHECK(r.fsk_confidence == p.fsk_confidence);
    CHECK(r.pulse_shape_used == p.pulse_shape_used);
    // belt and braces: re-serializing the copy gives the same bytes, so nothing was dropped
    CHECK(serialize_result(r) == bytes);
}

TEST_CASE("a real pipeline result crosses the wire and formats to identical text and JSON") {
    const auto raw = read_file(std::string(SIG_TEST_DATA_DIR) + "/pipeline/bpsk_ldpc_block.iq");
    PipelineOptions o;
    o.sample_rate = 8000.0;
    o.samples_per_symbol = 4;
    o.auto_detect = true;
    o.recover_timing = true;
    const PipelineResult p = run_pipeline(raw.data(), raw.size(), "x.iq", o, {"bpsk", 1.0});
    const auto bytes = serialize_result(p);
    const PipelineResult r = deserialize_result(bytes.data(), bytes.size());
    CHECK(format_result_text(r, SIZE_MAX, true, true) == format_result_text(p, SIZE_MAX, true, true));
    CHECK(format_result_json(r) == format_result_json(p));
    CHECK(r.samples == p.samples);
    CHECK(r.spectrogram.data == p.spectrogram.data);
}

TEST_CASE("malformed bytes are rejected, never read out of range") {
    const auto good = serialize_result(full_result());
    CHECK_THROWS(deserialize_result(nullptr, 0));
    CHECK_THROWS(deserialize_result(good.data(), 3));
    // every truncation of a valid buffer must throw (a prefix is never a valid result)
    for (size_t n = 0; n < good.size(); n += 7) CHECK_THROWS(deserialize_result(good.data(), n));
    auto trailing = good;
    trailing.push_back(0);
    CHECK_THROWS(deserialize_result(trailing.data(), trailing.size()));
    auto wrong_magic = good;
    wrong_magic[0] ^= 0xFF;
    CHECK_THROWS(deserialize_result(wrong_magic.data(), wrong_magic.size()));
    // a huge length prefix right after the magic and file_path length
    auto huge = good;
    const uint64_t big = ~0ull;
    std::memcpy(huge.data() + 4, &big, sizeof(big));
    CHECK_THROWS(deserialize_result(huge.data(), huge.size()));
    // options and classification refuse each other's bytes
    const auto ob = serialize_options(PipelineOptions{});
    CHECK_THROWS(deserialize_result(ob.data(), ob.size()));
    CHECK_THROWS(deserialize_classification(ob.data(), ob.size()) /* options bytes are not a classification */);
}

TEST_MAIN()
