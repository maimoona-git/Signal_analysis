// Phase 6: blind interleaver/FEC detection, re-implemented in C++ and validated
// against the ORIGINAL app/detect.py on 90 recorded cases (tests/data/pipeline/
// detect.rec): the full FEC x interleaver transmit grid, the native app's own
// scenarios, noisy channels, degenerate and odd-sized streams, and parameter
// sweeps. Every reported field must match: chosen interleaver and parameters,
// FEC and message length, score, and the recovered bit arrays -- including where
// the original is wrong (it misidentifies some LDPC/RS + convolutional streams;
// the port must be wrong in exactly the same way).

#include <string>
#include <vector>

#include "detect.hpp"
#include "../harness.hpp"
#include "../record_file.hpp"

using namespace sigapp;
using namespace sigtest;

#ifndef SIG_TEST_DATA_DIR
#error "SIG_TEST_DATA_DIR must point at tests/data"
#endif

namespace {

std::string params_string(const InterleaverSpec& s) {
    std::string out;
    auto add = [&](const char* k, const std::optional<uint32_t>& v) {
        if (!v) return;
        out += (out.empty() ? "" : ",") + std::string(k) + "=" + std::to_string(*v);
    };
    add("rows", s.rows); add("cols", s.cols); add("seed", s.seed);
    add("branches", s.branches); add("delay_increment", s.delay_increment);
    return out.empty() ? "-" : out;
}

}  // namespace

TEST_CASE("detect_params reproduces the original on all recorded cases") {
    const auto recs = read_records(std::string(SIG_TEST_DATA_DIR) + "/pipeline/detect.rec");
    Mismatches m;
    size_t detected = 0, undetected = 0, errors = 0;
    for (const auto& r : recs) {
        const std::string name = field(r, "name");
        const auto bits = bits_of(field(r, "bits"));
        std::optional<size_t> header_len;
        if (!is_none(field(r, "header_len"))) header_len = static_cast<size_t>(to_int(field(r, "header_len")));
        const int floor = static_cast<int>(to_int(field(r, "score_floor")));
        const auto seed_range = static_cast<unsigned int>(to_int(field(r, "seed_range")));

        if (has(r, "error")) {
            ++errors;
            bool threw = false;
            try { detect_params(bits, header_len, floor, seed_range); } catch (const std::invalid_argument&) { threw = true; }
            m.expect(threw, name, "original raised " + field(r, "error"));
            continue;
        }

        const DetectionResult got = detect_params(bits, header_len, floor, seed_range);
        const std::string got_kind = got.interleaver ? got.interleaver->kind : "none";
        m.equal(got_kind, field(r, "interleaver"), name, "interleaver");
        m.equal(got.interleaver ? params_string(*got.interleaver) : std::string("-"), field(r, "iparams"), name, "interleaver params");
        m.equal(got.fec.value_or("none"), field(r, "fec"), name, "fec");
        m.equal(got.fec_msg_len ? std::to_string(*got.fec_msg_len) : std::string("none"), field(r, "msg_len"), name, "fec_msg_len");
        m.equal(got.score, to_int(field(r, "score")), name, "score");
        m.equal(got.deinterleaved_bits ? bits_str(*got.deinterleaved_bits) : std::string("none"), field(r, "deinterleaved"), name, "deinterleaved_bits");
        m.equal(got.decoded_bits ? bits_str(*got.decoded_bits) : std::string("none"), field(r, "decoded"), name, "decoded_bits");
        m.equal(bits_str(got.working_bits), field(r, "working"), name, "working_bits");
        (got.interleaver || got.fec ? detected : undetected)++;
    }
    CHECK(recs.size() == 90);
    CHECK(errors == 1);
    CHECK(detected >= 50);     // the suite really exercises successful detections...
    CHECK(undetected >= 10);   // ...and the "nothing found" outcomes
    m.finish(recs.size());
}

TEST_CASE("detect_params on a clean framed stream finds the transmit parameters") {
    // Independent of the recorded data: a stream with a repeating 16-bit header,
    // block-interleaved and LDPC-coded, must be unwound exactly.
    const auto recs = read_records(std::string(SIG_TEST_DATA_DIR) + "/pipeline/detect.rec");
    for (const auto& r : recs) {
        if (field(r, "name") != "native_block_ldpc") continue;
        const auto got = detect_params(bits_of(field(r, "bits")));
        CHECK(got.interleaver.has_value() && got.interleaver->kind == "block");
        CHECK(*got.interleaver == InterleaverSpec::block(16, 48));
        CHECK(got.fec == std::optional<std::string>("ldpc"));
        CHECK(got.fec_msg_len == std::optional<size_t>(384));
        return;
    }
    CHECK(false);  // record not found
}

TEST_CASE("detect_params rejects empty input and honours the score floor") {
    CHECK_THROWS(detect_params({}));
    const auto recs = read_records(std::string(SIG_TEST_DATA_DIR) + "/pipeline/detect.rec");
    for (const auto& r : recs) {
        if (field(r, "name") != "native_noise256") continue;
        const auto bits = bits_of(field(r, "bits"));
        const auto none = detect_params(bits);
        CHECK(!none.interleaver && !none.fec && none.score < 3 && none.working_bits == bits);
        return;
    }
    CHECK(false);
}

TEST_MAIN()
