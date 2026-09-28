// Feature 1 (SIH26147 extra feature, descoped): decode-driven parameter search.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "fec.hpp"
#include "fec_dispatch.hpp"
#include "param_search.hpp"
#include "../harness.hpp"
#include "../testutil.hpp"

using namespace sigapp;
using namespace sigtest;

namespace {

// See test_rank_analysis.cpp for why: testutil's Rng is a xorshift generator, LINEAR
// over GF(2), which is exactly the wrong kind of "random" for anything that measures
// real structure (there, GF(2) rank; here, a genuine FEC round-trip match can
// coincidentally look stronger against linear-complexity-bounded "noise" than it
// should). A splitmix64-style nonlinear mix avoids that confound.
uint64_t nonlinear_mix(uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}
Bits nonlinear_bits(size_t n, uint64_t seed) {
    Bits v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(nonlinear_mix(seed * 0x9E3779B97F4A7C15ull + i) & 1);
    return v;
}

}  // namespace

TEST_CASE("a single, non-repeating LDPC-encoded message beats 'no FEC' -- the exact bug this feature fixes") {
    // header_repeat_score cannot help here at all (nothing repeats), which is exactly
    // the scenario where detect.hpp's own heuristic was found (during this port's
    // testing) to pick the wrong answer: a coincidental header-repeat count on the
    // raw bits beating a real, verifiable FEC round-trip match.
    const auto msg = nonlinear_bits(64, 11);  // 8 LDPC(16,8) blocks
    Bits coded;
    for (size_t b = 0; b < msg.size() / 8; ++b) {
        const auto block = sigcore::ldpc_encode(Bits(msg.begin() + static_cast<long>(b * 8), msg.begin() + static_cast<long>(b * 8 + 8)));
        coded.insert(coded.end(), block.begin(), block.end());
    }

    const auto r = search_params(coded);
    CHECK(r.detected);
    const auto& best = r.candidates[r.best_index];
    CHECK(best.fec.has_value());
    CHECK(*best.fec == "ldpc");
    CHECK(best.reencode_agreement.has_value());
    CHECK(*best.reencode_agreement > 0.99);

    // The "no FEC" candidate for the same (no-interleaver) branch is also in the
    // list, ranked below the real decode, with the noticeably lower, capped
    // confidence a mere header-repeat count deserves here.
    bool found_no_fec = false;
    for (const auto& c : r.candidates) {
        if (!c.interleaver.has_value() && !c.fec.has_value()) {
            found_no_fec = true;
            CHECK(c.confidence < best.confidence);
            CHECK(c.confidence <= 0.45);
        }
    }
    CHECK(found_no_fec);
}

TEST_CASE("a real decode is confidently detected for every FEC scheme, not just LDPC") {
    // Confirms the per-scheme noise-baseline calibration (param_search.cpp's
    // noise_baseline) does not overcorrect: a genuine, error-free decode still reads
    // near 1.0 for viterbi, rs and concatenated too, not just the scheme the main bug
    // report happened to involve.
    for (const std::string fec : {"viterbi", "rs", "concatenated"}) {
        const size_t msg_len = fec == "viterbi" ? 40 : fec == "rs" ? 13 : 13;  // bits for viterbi, bytes for rs/concatenated
        const auto msg = fec == "viterbi" ? nonlinear_bits(msg_len, 51) : nonlinear_bits(msg_len * 8, 51);
        const auto coded = fec_encode(msg, fec);
        const auto r = search_params(coded);
        CHECK(r.detected);
        const auto& best = r.candidates[r.best_index];
        CHECK(best.fec.has_value());
        // A concatenated-encoded stream is, structurally, ALSO a valid plain
        // Viterbi-encoded stream (its "message" is just the RS-encoded bytes,
        // which is exactly what viterbi_decode would correctly recover): both
        // readings genuinely round-trip, so either is an honest top answer.
        CHECK((*best.fec == fec || (fec == "concatenated" && *best.fec == "viterbi")));
        CHECK(*best.reencode_agreement > 0.99);
        CHECK(best.confidence > 0.9);
    }
}

TEST_CASE("a strongly repeating header with no real FEC is capped well below what a real decode reports") {
    // A header that repeats often gives the no-FEC candidate a large header_repeat_score,
    // but that is still only circumstantial evidence (nothing was actually verified to
    // decode correctly) -- confidence must stay capped, not creep toward 1.0 just
    // because the count is large.
    // A 16-bit header, matching search_params' own hlen formula (min(16, max(4, n/8)))
    // for this stream's size, so the repeat is actually visible to header_repeat_score.
    const Bits header = {1, 0, 1, 1, 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 0, 1};
    Bits stream;
    for (int i = 0; i < 40; ++i) {
        stream.insert(stream.end(), header.begin(), header.end());
        const auto payload = nonlinear_bits(24, static_cast<uint64_t>(i) + 500);
        stream.insert(stream.end(), payload.begin(), payload.end());
    }
    const auto r = search_params(stream);
    bool found = false;
    for (const auto& c : r.candidates) {
        if (!c.interleaver.has_value() && !c.fec.has_value() && c.header_repeat_score >= 8) {
            found = true;
            CHECK(c.confidence <= 0.45);
        }
    }
    CHECK(found);
}

TEST_CASE("plain noise never claims a confident detection") {
    const auto bits = nonlinear_bits(2000, 3);
    const auto r = search_params(bits);
    CHECK(!r.detected);
    for (const auto& c : r.candidates) CHECK(c.confidence < 0.5);
}

TEST_CASE("candidates are sorted by confidence descending, and capped") {
    const auto msg = nonlinear_bits(64, 21);
    Bits coded;
    for (size_t b = 0; b < msg.size() / 8; ++b) {
        const auto block = sigcore::ldpc_encode(Bits(msg.begin() + static_cast<long>(b * 8), msg.begin() + static_cast<long>(b * 8 + 8)));
        coded.insert(coded.end(), block.begin(), block.end());
    }
    ParamSearchOptions opts;
    opts.max_candidates_kept = 5;
    const auto r = search_params(coded, opts);
    CHECK(r.candidates.size() <= 5);
    CHECK(std::is_sorted(r.candidates.begin(), r.candidates.end(),
                         [](const ParamCandidate& a, const ParamCandidate& b) { return a.confidence > b.confidence; }));
}

TEST_CASE("max_bit_offset finds the true start when the stream has junk bits prepended") {
    const auto msg = nonlinear_bits(8, 31);
    const auto coded = sigcore::ldpc_encode(msg);  // 16 bits, one clean LDPC(16,8) block
    Bits junk = nonlinear_bits(5, 32);
    Bits stream = junk;
    stream.insert(stream.end(), coded.begin(), coded.end());

    ParamSearchOptions opts;
    opts.max_bit_offset = 8;  // covers the 5-bit junk prefix
    const auto r = search_params(stream, opts);
    CHECK(r.detected);
    const auto& best = r.candidates[r.best_index];
    CHECK(best.bit_offset == 5);
    CHECK(best.fec.has_value() && *best.fec == "ldpc");
    CHECK(*best.reencode_agreement > 0.99);
}

TEST_CASE("without an offset search, the same misaligned stream is not found this way") {
    const auto msg = nonlinear_bits(8, 41);
    const auto coded = sigcore::ldpc_encode(msg);
    Bits junk = nonlinear_bits(5, 42);
    Bits stream = junk;
    stream.insert(stream.end(), coded.begin(), coded.end());

    const auto r = search_params(stream);  // max_bit_offset defaults to 0
    for (const auto& c : r.candidates) CHECK(c.bit_offset == 0);
}

TEST_CASE("a capture too short to even be candidate data reports not detected, not a guess") {
    const Bits bits(3, 1);
    const auto r = search_params(bits);
    CHECK(!r.detected);
}

TEST_CASE("empty input throws") {
    bool threw = false;
    try {
        search_params({});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

TEST_CASE("decode_candidate reproduces exactly what search_params scored, for the winning candidate") {
    const auto msg = nonlinear_bits(64, 61);
    Bits coded;
    for (size_t b = 0; b < msg.size() / 8; ++b) {
        const auto block = sigcore::ldpc_encode(Bits(msg.begin() + static_cast<long>(b * 8), msg.begin() + static_cast<long>(b * 8 + 8)));
        coded.insert(coded.end(), block.begin(), block.end());
    }
    const auto r = search_params(coded);
    CHECK(r.detected);
    const auto& best = r.candidates[r.best_index];
    const auto decoded = decode_candidate(coded, best);
    CHECK(decoded.size() == best.decoded_bits);
    CHECK(decoded == msg);  // the real message comes back, not a placeholder
}

TEST_CASE("decode_candidate: a bit_offset at or past the end throws") {
    bool threw = false;
    try {
        decode_candidate(Bits(4, 1), ParamCandidate{4, std::nullopt, std::nullopt, std::nullopt, std::nullopt, 0, 0.0, 0});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

TEST_MAIN()
