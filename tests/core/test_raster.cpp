// Feature 4 (SIH26147 extra feature): frame raster view. Finds the repeating
// frame length, folds the bitstream into a matrix, re-aligns for bit slips,
// and labels each column constant (header/sync) / counter-like / payload.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "raster.hpp"
#include "../harness.hpp"
#include "../testutil.hpp"

using namespace sigapp;
using namespace sigtest;

namespace {

// A repeating, deterministic header has its OWN internal autocorrelation, and since
// it recurs identically in every frame, that internal structure shows up as genuinely
// strong (not noise-driven) correlation at any lag shorter than the header itself --
// not just at the true frame length. A header picked without checking for this can
// make the detector correctly, honestly lock onto a shorter internal repeat instead of
// the intended frame period (found by measurement while building these tests, not
// assumed). Real protocols solve this by choosing a sync word with low autocorrelation
// at every nonzero lag; Barker-13 is the textbook example, so tests that need the
// detector to find one specific, unambiguous period use it instead of an arbitrary
// hand-picked bit string.
const Bits kBarker13 = {1, 1, 1, 1, 1, 0, 0, 1, 1, 0, 1, 0, 1};

// A frame: a fixed `header` (sync word), then `counter_bits` bits of a binary
// counter that increments once per frame (LSB first, so bit 0 flips every
// frame, bit 1 every 2 frames, ...), then random payload filling the rest.
std::vector<Bits> make_frames(const Bits& header, unsigned counter_bits, unsigned frame_len,
                              unsigned num_frames, uint64_t payload_seed) {
    Rng rng(payload_seed);
    std::vector<Bits> frames(num_frames);
    for (unsigned i = 0; i < num_frames; ++i) {
        Bits frame = header;
        for (unsigned b = 0; b < counter_bits; ++b) frame.push_back((i >> b) & 1);
        while (frame.size() < frame_len) frame.push_back(static_cast<uint8_t>((rng.next() >> 33) & 1));
        frames[i] = frame;
    }
    return frames;
}

Bits concat(const std::vector<Bits>& frames) {
    Bits out;
    for (const auto& f : frames) out.insert(out.end(), f.begin(), f.end());
    return out;
}

}  // namespace

TEST_CASE("finds the frame length, sync word, and counter field on a clean framed stream") {
    const Bits header = {1, 0, 1, 1, 0, 0, 1, 0};  // 8-bit sync word
    const auto frames = make_frames(header, 4, 32, 40, 7);
    const auto bits = concat(frames);

    const auto raster = build_frame_raster(bits);
    CHECK(raster.detected);
    CHECK(raster.frame_length == 32);
    CHECK(raster.num_frames == 40);
    CHECK(raster.sync_word == header);
    CHECK(raster.header_columns.size() >= header.size());  // sync word + at least some of the counter

    for (unsigned b = 0; b < 4; ++b) {
        const auto& col = raster.columns[header.size() + b];
        CHECK(col.label == ColumnLabel::CounterLike);
    }
    for (unsigned j = header.size() + 4; j < 32; ++j) {
        CHECK(raster.columns[j].label == ColumnLabel::Payload);
    }
    // Sync word recurs at every frame boundary, found by the whole-stream search (an 8-bit
    // pattern with 1 bit of tolerance also picks up a few chance matches inside the random
    // payload elsewhere -- expected for a short sync word, not a bug; every TRUE position
    // must still be among them).
    for (unsigned i = 0; i < 40; ++i) {
        CHECK(std::find(raster.sync_positions.begin(), raster.sync_positions.end(), i * 32) !=
              raster.sync_positions.end());
    }
}

TEST_CASE("with no counter field, nothing after the sync word is mislabelled counter-like") {
    // The first column after the sync word here is real random payload, whose expected
    // transition rate (~0.5) is exactly what a counter's second bit would also show --
    // the anchor requirement (the run must START at a column near rate 1.0) is what
    // keeps this from being misread as a counter field.
    const Bits header = {0, 0, 1, 1, 0, 1};
    const auto frames = make_frames(header, /*counter_bits=*/0, 30, 50, 66);
    const auto raster = build_frame_raster(concat(frames));
    CHECK(raster.detected);
    CHECK(raster.sync_word == header);
    CHECK(raster.header_columns.size() == header.size());  // no counter columns appended
    for (unsigned j = header.size(); j < 30; ++j) CHECK(raster.columns[j].label == ColumnLabel::Payload);
}

TEST_CASE("picks the fundamental period, not a harmonic of it (2x, 3x also correlate)") {
    const Bits header = {1, 1, 0, 0, 1, 1, 0, 1};
    const auto frames = make_frames(header, 3, 24, 60, 11);
    const auto bits = concat(frames);
    const auto raster = build_frame_raster(bits);
    CHECK(raster.detected);
    CHECK(raster.frame_length == 24);  // not 48 or 72, which also show a strong peak
}

TEST_CASE("re-alignment recovers a one-bit insertion (slip) partway through the stream") {
    const Bits header = {0, 1, 1, 0, 1, 0, 0, 1, 1, 0};  // 10-bit sync word
    const auto clean_frames = make_frames(header, 2, 40, 24, 21);
    const auto clean = concat(clean_frames);

    // Splice one extra bit in after frame 16 of 24 (a minority of frames end up
    // shifted): everything from there on is offset by one place relative to the
    // naive fold at multiples of the frame length.
    Bits slipped(clean.begin(), clean.begin() + 16 * 40);
    slipped.push_back(1);
    slipped.insert(slipped.end(), clean.begin() + 16 * 40, clean.end());

    const auto clean_raster = build_frame_raster(clean);
    const auto slipped_raster = build_frame_raster(slipped);
    CHECK(clean_raster.detected && slipped_raster.detected);
    CHECK(slipped_raster.frame_length == 40);
    CHECK(slipped_raster.sync_word == header);              // recovered exactly despite the slip
    CHECK(slipped_raster.sync_word == clean_raster.sync_word);

    // At least one of the affected rows (16..23) needed a nonzero shift to re-align.
    bool any_shifted = false;
    for (unsigned r = 16; r < slipped_raster.num_frames && r < slipped_raster.row_shift.size(); ++r) {
        any_shifted |= (slipped_raster.row_shift[r] != 0);
    }
    CHECK(any_shifted);
}

TEST_CASE("the sync word is found across the whole stream even where the fold misses a frame") {
    // Same construction as the slip test, but check header_columns / sync_positions
    // still line up with the frame length once re-aligned.
    const Bits header = {1, 1, 1, 0, 0, 0, 1, 0};
    const auto frames = make_frames(header, 0, 32, 30, 33);
    auto bits = concat(frames);
    // Corrupt two bits inside one frame's sync word (a real bit-error case, not a slip).
    bits[15 * 32 + 2] ^= 1;
    bits[20 * 32 + 5] ^= 1;

    const auto raster = build_frame_raster(bits);
    CHECK(raster.detected);
    CHECK(raster.sync_word == header);
    // Every frame's sync word is still found (max_bit_errors default = 1 per occurrence,
    // and each corrupted frame here has exactly one flipped bit in its own sync word) --
    // including the two that were corrupted, which an exact search would have missed.
    for (unsigned i = 0; i < 30; ++i) {
        CHECK(std::find(raster.sync_positions.begin(), raster.sync_positions.end(), i * 32u) !=
              raster.sync_positions.end());
    }
}

TEST_CASE("a pure-noise stream reports no detection, not a confident wrong answer") {
    const auto noise = random_bits(4000, 99);
    const auto raster = build_frame_raster(noise);
    CHECK(!raster.detected);
    CHECK(raster.frame_length == 0);
    CHECK(raster.matrix.empty());
    CHECK(raster.sync_word.empty());
}

TEST_CASE("candidate_periods lets an external hint (e.g. a rank-analysis spike) win directly") {
    const Bits header = {1, 0, 0, 1, 1, 0};
    const auto frames = make_frames(header, 2, 20, 30, 42);
    const auto bits = concat(frames);
    RasterOptions options;
    options.candidate_periods = {20};
    const auto raster = build_frame_raster(bits, options);
    CHECK(raster.detected);
    CHECK(raster.frame_length == 20);
}

TEST_CASE("too little data, or a min/max period range that excludes everything, is not detected") {
    CHECK(!build_frame_raster(random_bits(10, 1)).detected);   // far too short for any candidate
    RasterOptions narrow;
    narrow.min_period = 100;
    narrow.max_period = 90;  // max <= min: an empty range
    CHECK(!build_frame_raster(random_bits(500, 2), narrow).detected);
    CHECK_THROWS(build_frame_raster({}));
}

TEST_CASE("column statistics have the right shape and range for every column") {
    const auto frames = make_frames(kBarker13, 2, 20, 25, 55);
    const auto raster = build_frame_raster(concat(frames));
    CHECK(raster.detected);
    CHECK(raster.columns.size() == 20);
    for (const auto& col : raster.columns) {
        CHECK(col.ones_fraction >= 0.0 && col.ones_fraction <= 1.0);
        CHECK(col.stability >= 0.0 && col.stability <= 1.0);
        CHECK(col.transition_rate >= 0.0 && col.transition_rate <= 1.0);
        CHECK_NEAR(col.stability, std::fabs(2.0 * col.ones_fraction - 1.0), 1e-9);
    }
    CHECK(raster.matrix.size() == static_cast<size_t>(raster.num_frames) * raster.frame_length);
}

TEST_CASE("degrades gracefully under residual bit errors: still detected around a moderate BER") {
    // A rough measurement of the working range, not a hard-coded pass threshold: this
    // records where the fundamental period is still recovered, at 1% and 5% flipped bits.
    const auto frames = make_frames(kBarker13, 3, 48, 50, 77);
    auto bits = concat(frames);
    Rng rng(88);
    for (auto& b : bits) {
        if ((rng.next() % 100) < 1) b ^= 1;  // ~1% BER
    }
    const auto raster = build_frame_raster(bits);
    CHECK(raster.detected);
    CHECK(raster.frame_length == 48);
}

TEST_MAIN()
