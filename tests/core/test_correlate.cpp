// Phase 6: correlate.cpp must agree with the ORIGINAL app/correlate.py on 139
// recorded cases (tests/data/pipeline/correlate.rec): exact autocorrelation
// values, frame periods (including tie-breaking), pattern search, bits-to-text,
// and header/payload boundaries -- including every case where the original raises.

#include <cmath>
#include <string>
#include <vector>

#include "correlate.hpp"
#include "../harness.hpp"
#include "../record_file.hpp"
#include "../testutil.hpp"

using namespace sigapp;
using namespace sigtest;

#ifndef SIG_TEST_DATA_DIR
#error "SIG_TEST_DATA_DIR must point at tests/data"
#endif

namespace {

std::vector<Record> records() { return read_records(std::string(SIG_TEST_DATA_DIR) + "/pipeline/correlate.rec"); }

std::string hex_of(const std::string& s) {
    static const char* d = "0123456789abcdef";
    std::string out;
    for (unsigned char c : s) { out += d[c >> 4]; out += d[c & 15]; }
    return out.empty() ? "-" : out;
}

template <class F>
bool throws_invalid_argument(F&& f) {
    try { f(); } catch (const std::invalid_argument&) { return true; } catch (...) { return false; }
    return false;
}

}  // namespace

TEST_CASE("autocorrelation is bit-identical to the original") {
    Mismatches m;
    size_t n = 0;
    for (const auto& r : records()) {
        if (field(r, "op") != "autocorr") continue;
        ++n;
        const auto bits = bits_of(field(r, "bits"));
        if (has(r, "error")) { m.expect(throws_invalid_argument([&] { autocorrelation(bits); }), field(r, "name"), "should throw"); continue; }
        const auto got = autocorrelation(bits);
        const auto want = split(field(r, "lags"), ',');
        m.equal(got.size(), want.size(), field(r, "name"), "length");
        for (size_t i = 0; i < std::min(got.size(), want.size()); ++i) {
            if (got[i] != to_double(want[i])) { m.expect(false, field(r, "name"), "lag " + std::to_string(i + 1) + " differs"); break; }
        }
    }
    CHECK(n == 11);
    m.finish(n);
}

TEST_CASE("packed-word autocorrelation equals the naive O(n^2) sum on random streams") {
    for (size_t n : {2u, 3u, 63u, 64u, 65u, 127u, 128u, 129u, 500u, 1000u}) {
        const auto bits = random_bits(n, n);
        const auto fast = autocorrelation(bits);
        for (size_t lag = 1; lag < n; ++lag) {
            long long sum = 0;
            for (size_t i = 0; i + lag < n; ++i) sum += (2 * bits[i] - 1) * (2 * bits[i + lag] - 1);
            CHECK(fast[lag - 1] == static_cast<double>(sum) / static_cast<double>(n));
        }
    }
}

TEST_CASE("detect_frame_period matches the original, ties included") {
    Mismatches m;
    size_t n = 0;
    for (const auto& r : records()) {
        if (field(r, "op") != "frame_period") continue;
        ++n;
        const auto bits = bits_of(field(r, "bits"));
        const int min_p = static_cast<int>(to_int(field(r, "min_period")));
        std::optional<int> max_p;
        if (!is_none(field(r, "max_period"))) max_p = static_cast<int>(to_int(field(r, "max_period")));
        const std::string label = field(r, "name") + " min=" + field(r, "min_period") + " max=" + field(r, "max_period");
        if (has(r, "error")) { m.expect(throws_invalid_argument([&] { detect_frame_period(bits, min_p, max_p); }), label, "should throw"); continue; }
        const auto [period, strength] = detect_frame_period(bits, min_p, max_p);
        m.equal(period, to_int(field(r, "period")), label, "period");
        m.expect(strength == to_double(field(r, "strength")), label, "strength");
    }
    CHECK(n == 77);
    m.finish(n);
}

TEST_CASE("find_pattern_occurrences matches the original") {
    Mismatches m;
    size_t n = 0;
    for (const auto& r : records()) {
        if (field(r, "op") != "find") continue;
        ++n;
        const auto got = find_pattern_occurrences(bits_of(field(r, "bits")), bits_of(field(r, "pattern")),
                                                  static_cast<int>(to_int(field(r, "max_errors"))));
        std::vector<size_t> want;
        for (const auto& s : split(field(r, "positions"), ',')) want.push_back(static_cast<size_t>(to_int(s)));
        m.equal(got, want, field(r, "name") + " errs=" + field(r, "max_errors"), "positions");
    }
    CHECK(n == 9);
    m.finish(n);
}

TEST_CASE("bits_to_text matches the original (text, hex fallback, ragged tail)") {
    Mismatches m;
    size_t n = 0;
    for (const auto& r : records()) {
        if (field(r, "op") != "text") continue;
        ++n;
        // The original returns str; the fixture stores its UTF-8 bytes as hex.
        m.equal(hex_of(bits_to_text(bits_of(field(r, "bits")))), field(r, "expected_hex"), field(r, "name"), "text");
    }
    CHECK(n == 14);
    m.finish(n);
}

TEST_CASE("find_header_payload_boundaries matches the original") {
    Mismatches m;
    size_t n = 0;
    for (const auto& r : records()) {
        if (field(r, "op") != "boundaries") continue;
        ++n;
        const std::string name = field(r, "name");
        const auto bits = bits_of(field(r, "bits"));
        std::optional<Bits> pattern;
        if (!is_none(field(r, "pattern"))) pattern = bits_of(field(r, "pattern"));
        const size_t hlen = static_cast<size_t>(to_int(field(r, "header_len")));
        const int errs = static_cast<int>(to_int(field(r, "max_errors")));
        const int minp = static_cast<int>(to_int(field(r, "min_period")));
        if (has(r, "error")) {
            m.expect(throws_invalid_argument([&] { find_header_payload_boundaries(bits, pattern, hlen, errs, minp); }), name, "should throw");
            continue;
        }
        const auto got = find_header_payload_boundaries(bits, pattern, hlen, errs, minp);
        m.equal(got.header_pattern, bits_of(field(r, "out_pattern")), name, "header_pattern");
        m.equal(got.frame_period.has_value() ? std::to_string(*got.frame_period) : std::string("none"), field(r, "frame_period"), name, "frame_period");
        std::vector<size_t> want_pos;
        for (const auto& s : split(field(r, "positions"), ',')) want_pos.push_back(static_cast<size_t>(to_int(s)));
        m.equal(got.header_positions, want_pos, name, "positions");
        std::vector<std::string> got_payloads, want_payloads = split(field(r, "payloads"), ',');
        for (const auto& p : got.payloads) got_payloads.push_back(bits_str(p));
        m.equal(got_payloads.size(), want_payloads.size(), name, "payload count");
        for (size_t i = 0; i < std::min(got_payloads.size(), want_payloads.size()); ++i)
            m.equal(got_payloads[i], want_payloads[i], name, "payload " + std::to_string(i));
    }
    CHECK(n == 28);
    m.finish(n);
}

TEST_MAIN()
