// Phase 7: the results-panel / status-line text, byte for byte against what the
// ORIGINAL PyQt6 ResultsPanel shows for the same result (recorded for every
// successful pipeline scenario in tests/data/pipeline/pipeline.rec).

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "result_text.hpp"
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

std::string from_hex(const std::string& hex) {
    std::string out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) out += static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16));
    return out;
}

PipelineOptions options_of(const Record& r) {
    PipelineOptions o;
    if (!is_none(field(r, "sample_rate"))) o.sample_rate = to_double(field(r, "sample_rate"));
    o.samples_per_symbol = static_cast<unsigned>(to_int(field(r, "samples_per_symbol")));
    for (const auto& [k, v] : params_of(field(r, "demod"))) {
        if (k == "bits_per_symbol") o.demod.bits_per_symbol = static_cast<unsigned>(to_int(v));
        if (k == "samples_per_symbol") o.demod.samples_per_symbol = static_cast<unsigned>(to_int(v));
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

}  // namespace

TEST_CASE("results text matches the original GUI panel on every successful scenario") {
    const auto recs = read_records(data_path("pipeline.rec"));
    Mismatches m;
    size_t compared = 0, with_detection = 0, with_decode = 0;
    for (const auto& r : recs) {
        if (!has(r, "panel_text_hex")) continue;
        ++compared;
        const std::string name = field(r, "name");
        const auto bytes = read_file(data_path(field(r, "file")));
        const auto result = run_pipeline(bytes.data(), bytes.size(), field(r, "file"), options_of(r),
                                         {field(r, "label"), to_double(field(r, "confidence"))});
        m.equal(format_result_text(result), from_hex(field(r, "panel_text_hex")), name, "panel text");
        m.equal(format_done_status(result), from_hex(field(r, "status_hex")), name, "status line");
        if (result.detection_score) ++with_detection;
        if (result.decoded_bits) ++with_decode;
    }
    CHECK(compared == 72);   // every scenario the original completes (84 minus its 12 errors)
    CHECK(with_detection >= 20);   // the "Detected ..." lines are exercised
    CHECK(with_decode >= 30);      // and the "FEC-decoded bits" line
    m.finish(compared);
}

TEST_CASE("format_g is Python's %g") {
    CHECK(format_g(8000.0) == "8000");
    CHECK(format_g(1e6) == "1e+06");
    CHECK(format_g(11025.0) == "11025");
    CHECK(format_g(0.1 + 0.2) == "0.3");
    CHECK(format_g(1234567.0) == "1.23457e+06");
    CHECK(format_g(1e-5) == "1e-05");
    CHECK(format_g(100000.0) == "100000");
    CHECK(format_g(2.5e9) == "2.5e+09");
}

TEST_CASE("python_repr follows Python's quoting and escaping") {
    CHECK(python_repr("SIH2026!") == "'SIH2026!'");
    CHECK(python_repr("") == "''");
    CHECK(python_repr("it's") == "\"it's\"");                 // has ' and no " -> double quotes
    CHECK(python_repr("say \"hi\"") == "'say \"hi\"'");
    CHECK(python_repr("it's \"both\"") == "'it\\'s \"both\"'");  // both -> single quotes, ' escaped
    CHECK(python_repr("a\\b") == "'a\\\\b'");
    CHECK(python_repr("l1\nl2\tx\r") == "'l1\\nl2\\tx\\r'");
    CHECK(python_repr(std::string("\x01\x7f", 2)) == "'\\x01\\x7f'");
    CHECK(python_repr("53494832") == "'53494832'");           // the hex fallback is just a string
}

TEST_CASE("the status line rounds the confidence like Python's :.0%") {
    PipelineResult r;
    r.predicted_modulation = "qpsk";
    r.predicted_confidence = 0.324;
    CHECK(format_done_status(r) == "Done. Predicted modulation: qpsk (32% confidence).");
    r.predicted_confidence = 0.5;   // Python: f"{0.005:.0%}"-style ties go to even
    CHECK(format_done_status(r) == "Done. Predicted modulation: qpsk (50% confidence).");
    r.predicted_confidence = 1.0;
    CHECK(format_done_status(r) == "Done. Predicted modulation: qpsk (100% confidence).");
    r.predicted_confidence = 0.005;
    CHECK(format_done_status(r) == "Done. Predicted modulation: qpsk (0% confidence).");
    r.predicted_confidence = 0.015;
    CHECK(format_done_status(r) == "Done. Predicted modulation: qpsk (2% confidence).");
}

TEST_CASE("a message longer than the limit is cut with a note; shorter or equal ones are untouched") {
    PipelineResult r;
    r.predicted_modulation = "bpsk";
    r.boundaries.header_pattern = {1, 0};
    r.demodulated_bits.assign(8 * 20, 1);              // 20 bytes of 0xFF -> not ASCII -> a 40-character hex string

    const std::string full = format_result_text(r);
    CHECK(full.find("Decoded message:      '" + std::string(40, 'f') + "'") != std::string::npos);
    CHECK(format_result_text(r, 40) == full);           // exactly at the limit: unchanged
    CHECK(format_result_text(r, 1000) == full);

    const std::string cut = format_result_text(r, 10);
    CHECK(cut.find("Decoded message:      '" + std::string(10, 'f') + "' ... (40 characters in all, the first 10 shown)") != std::string::npos);
    CHECK(cut.find(std::string(11, 'f')) == std::string::npos);         // nothing beyond the limit leaks
    // every other line is identical to the full text
    auto without_message = [](std::string t) {
        const auto a = t.find("Decoded message:");
        return t.substr(0, a) + t.substr(t.find('\n', a));
    };
    CHECK(without_message(cut) == without_message(full));
    CHECK(format_result_text(r, 0).find("'' ... (40 characters in all, the first 0 shown)") != std::string::npos);
}

TEST_MAIN()
