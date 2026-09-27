// Feature 2 (SIH26147 extra feature): rank-analysis view. Detects a linear block
// code's length blindly (before any decode attempt) from a rank-deficiency spike in
// a GF(2) rank profile.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "fec.hpp"
#include "rank_analysis.hpp"
#include "../harness.hpp"
#include "../testutil.hpp"

using namespace sigapp;
using namespace sigtest;

namespace {

// testutil.hpp's Rng is a xorshift generator: fast and fine for every OTHER test's
// purposes, but xorshift is a LINEAR map over GF(2) (only XOR and shift), so its
// output bitstream itself has bounded GF(2) rank (at most its ~64-bit state size),
// found while writing these tests, when it kept reporting spurious structure at every
// width, not just the code length. A rank-based detector is precisely the kind of
// statistical test that catches this (it is a real, known weakness of xorshift/LCG-style
// generators -- NIST's own test suites include a "linear complexity" test for exactly
// this reason), so it makes a bad source of "real random, no structure" data here. This
// splitmix64-style finalizer is nonlinear over GF(2) (its multiplications carry bits
// across positions), which is enough to give it the high linear complexity a genuine
// null case needs.
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

// Many independent LDPC(16,8) codewords (the real hand-rolled code in core/src/fec.cpp,
// not a toy): a genuine [16,8] linear block code with 8 redundant bits per block, rate
// 1/2 -- real headroom, unlike a code with only a couple of parity bits. The message
// bits come from nonlinear_bits, not testutil's Rng, for the same reason as above: a
// linear-complexity-bounded message would leave its own artefact at other widths too.
Bits ldpc_stream(unsigned num_blocks, uint64_t seed) {
    Bits out;
    for (unsigned i = 0; i < num_blocks; ++i) {
        const auto msg = nonlinear_bits(8, seed * 1000003u + i);
        const auto coded = sigcore::ldpc_encode(msg);  // 8 message bits -> 16 codeword bits
        out.insert(out.end(), coded.begin(), coded.end());
    }
    return out;
}

}  // namespace

TEST_CASE("finds the true codeword length of a clean LDPC(16,8) stream and its rate") {
    const auto bits = ldpc_stream(400, 1);  // 6400 bits, well above the min_width*9 floor

    const auto r = build_rank_analysis(bits);
    CHECK(r.detected);
    CHECK(r.width == 16);
    CHECK(r.rank <= 9.0);   // the true dimension is 8; a little slack for sampling noise
    CHECK(r.rank >= 7.0);
    CHECK(r.code_rate > 0.4);
    CHECK(r.code_rate < 0.65);
    // the profile covers every width tried, in order, and the winner is in it
    CHECK(!r.profile.empty());
    CHECK(r.profile.front().width == 2);
    bool found_in_profile = false;
    for (const auto& p : r.profile) {
        if (p.width == 16) { found_in_profile = true; CHECK(p.excess_deficiency >= 3.0); }
    }
    CHECK(found_in_profile);
}

TEST_CASE("a width that is NOT the code length shows no spike (random-baseline is a real floor, not zero)") {
    const auto bits = ldpc_stream(400, 2);
    const auto r = build_rank_analysis(bits);
    CHECK(r.detected);
    for (const auto& p : r.profile) {
        if (p.width % 16 != 0) {  // an exact multiple folds N codewords per row: still deficient, honestly so
            CHECK(p.excess_deficiency < 3.0);
        }
    }
}

TEST_CASE("baseline_deficiency is the known ~0.7869 random-matrix corank constant, not zero") {
    // Plenty of rows relative to width (rows >= width): the "forced" term is zero, so
    // baseline_deficiency is exactly the constant. A width picked to divide evenly
    // avoids leftover-bits noise in how many rows are available.
    const auto bits = nonlinear_bits(6400, 11);
    const auto r = build_rank_analysis(bits);
    bool checked = false;
    for (const auto& p : r.profile) {
        if (p.width == 64) {  // 100 rows: comfortably >= width
            CHECK(p.baseline_deficiency > 0.7);
            CHECK(p.baseline_deficiency < 0.9);
            checked = true;
        }
    }
    CHECK(checked);
}

TEST_CASE("baseline_deficiency also accounts for having fewer rows than width (rank can never exceed rows)") {
    // Only 3 codewords' worth of bits at width 400: rank is forced to at most 3, no
    // matter how the bits are structured, so a random stream must still show a large
    // "deficiency" here purely from having too few rows -- that has to be subtracted
    // out, or a width wider than the row count would look like a false spike.
    const auto bits = nonlinear_bits(1200, 12);  // 3 rows at width 400
    RankAnalysisOptions opts;
    opts.candidate_widths = {400};
    opts.max_width = 400;
    const auto r = build_rank_analysis(bits, opts);
    bool checked = false;
    for (const auto& p : r.profile) {
        if (p.width == 400) {
            // 3 rows, 400 columns: forced deficiency is 400 - 3 = 397, plus the constant.
            CHECK(p.baseline_deficiency > 397.0);
            CHECK(p.baseline_deficiency < 398.0);
            CHECK(p.excess_deficiency < 3.0);  // and so this is correctly NOT flagged as a spike
            checked = true;
        }
    }
    CHECK(checked);
}

TEST_CASE("plain random bits (no code at all) never trip the detector") {
    const auto bits = nonlinear_bits(6400, 3);
    const auto r = build_rank_analysis(bits);
    CHECK(!r.detected);
    for (const auto& p : r.profile) CHECK(p.excess_deficiency < 3.0);
}

TEST_CASE("candidate_widths lets a caller point the search at a width it already suspects") {
    const auto bits = ldpc_stream(400, 4);
    RankAnalysisOptions opts;
    // 32 is a real (if less fundamental) spike here too -- an exact multiple of the true
    // width 16 -- so preferring it over what the general sweep would otherwise pick
    // first (16, the smaller and more fundamental one) actually exercises the
    // preference order, not just "whichever width happens to win anyway".
    opts.candidate_widths = {32};
    const auto r = build_rank_analysis(bits, opts);
    CHECK(r.detected);
    CHECK(r.width == 32);
}

TEST_CASE("a capture too short for even the smallest candidate width reports not detected, not a guess") {
    const Bits bits(10, 1);  // far fewer than min_width(2) * 9
    const auto r = build_rank_analysis(bits);
    CHECK(!r.detected);
    CHECK(r.profile.empty());
}

TEST_CASE("empty input throws") {
    bool threw = false;
    try {
        build_rank_analysis({});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

TEST_CASE("window_rows enables the median-of-windows mode, and one clean window is enough to detect") {
    const auto bits = ldpc_stream(400, 5);  // 400 blocks of 16 bits = 25 windows of 16 blocks
    RankAnalysisOptions opts;
    opts.window_rows = 16;  // 16 codewords per window: enough rows to reach the true rank 8
    opts.max_width = 64;
    const auto r = build_rank_analysis(bits, opts);
    CHECK(r.detected);
    CHECK(r.width == 16);
    CHECK(r.windows_used == 25);
}

TEST_CASE("median-of-windows survives a minority of corrupted windows outvoting the rest") {
    // 20 clean windows of 16 blocks, then corrupt every row of 4 more windows with a
    // random single-bit flip each -- a clear minority (4 of 24).
    Bits bits;
    for (unsigned w = 0; w < 20; ++w) {
        const auto block = ldpc_stream(16, 100 + w);
        bits.insert(bits.end(), block.begin(), block.end());
    }
    Rng rng(999);
    for (unsigned w = 0; w < 4; ++w) {
        auto block = ldpc_stream(16, 200 + w);
        for (unsigned row = 0; row < 16; ++row) {
            const size_t pos = row * 16 + (rng.next() % 16);
            block[pos] ^= 1;
        }
        bits.insert(bits.end(), block.begin(), block.end());
    }

    RankAnalysisOptions opts;
    opts.window_rows = 16;
    opts.max_width = 64;
    const auto r = build_rank_analysis(bits, opts);
    CHECK(r.detected);
    CHECK(r.width == 16);
    CHECK(r.windows_used == 24);
}

TEST_CASE("without windowing, the same corrupted minority can already wash out the spike (honest limit)") {
    // The exact same stream as above, but window_rows=0 (one window, all rows): every
    // corrupted row lands in the SAME single window as the clean ones, so there is no
    // majority to outvote it in. This documents the limitation rank_analysis.hpp
    // describes, rather than asserting a specific number (the point is that it is
    // measurably worse, not that it always fails).
    Bits clean = ldpc_stream(320, 300);
    Bits corrupted;
    for (unsigned w = 0; w < 20; ++w) {
        const auto block = ldpc_stream(16, 100 + w);
        corrupted.insert(corrupted.end(), block.begin(), block.end());
    }
    Rng rng(999);
    for (unsigned w = 0; w < 4; ++w) {
        auto block = ldpc_stream(16, 200 + w);
        for (unsigned row = 0; row < 16; ++row) {
            const size_t pos = row * 16 + (rng.next() % 16);
            block[pos] ^= 1;
        }
        corrupted.insert(corrupted.end(), block.begin(), block.end());
    }

    RankAnalysisOptions single_window;
    single_window.max_width = 64;  // window_rows stays 0: one window over everything
    const auto r_clean = build_rank_analysis(clean, single_window);
    const auto r_corrupted = build_rank_analysis(corrupted, single_window);
    CHECK(r_clean.detected);
    long clean_excess = 0, corrupted_excess = 0;
    for (const auto& p : r_clean.profile) if (p.width == 16) clean_excess = static_cast<long>(p.excess_deficiency);
    for (const auto& p : r_corrupted.profile) if (p.width == 16) corrupted_excess = static_cast<long>(p.excess_deficiency);
    CHECK(corrupted_excess < clean_excess);
}

TEST_MAIN()
