#pragma once

// Feature 4 (frame raster view, SIH26147 extra feature): finds the frame
// length from where a bitstream repeats, folds it into a matrix (rows =
// frames, columns = bit positions), re-aligns rows to correct bit slips, then
// measures how constant each column is across frames — a constant column is
// header/sync, a column that flips at a halving rate is a binary counter
// field, everything else is payload. The longest constant run at the start of
// the frame is taken as the sync word, and the whole original stream is then
// searched for it (tolerant of a few bit errors) to mark every frame
// boundary. See app/pipeline/README.md for the write-up and its honest limits,
// including a real one found while testing this: a repeating header has its OWN
// internal autocorrelation, and since it recurs identically in every frame, a
// header that isn't chosen with low self-correlation (a "good" sync word, e.g. a
// Barker code) can make frame-length detection lock onto that shorter internal
// repeat instead of the true frame length -- honestly, not a wrong-but-confident
// guess, just the wrong (real) periodicity.
//
// Pure C++, no liquid-dsp: builds and runs identically natively and on WASM.

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "bits.hpp"

namespace sigapp {

enum class ColumnLabel { Constant, CounterLike, Payload };

struct RasterColumn {
    double ones_fraction = 0.0;    // p_j: fraction of frames with a 1 in this column
    double stability = 0.0;        // s_j = |2p_j - 1|; 1 = always the same value, 0 = random
    double transition_rate = 0.0;  // t_j: fraction of consecutive-frame bit flips
    ColumnLabel label = ColumnLabel::Payload;
};

struct FrameRaster {
    // False when no repeating frame structure was found: everything else is left at its
    // default (empty), rather than reporting a low-confidence guess as if it were real.
    bool detected = false;
    unsigned frame_length = 0;  // F
    unsigned num_frames = 0;    // R = bits.size() / F

    // R x F, row-major, 0/1, AFTER per-row re-alignment (see row_shift).
    Bits matrix;
    // The shift applied to each row while re-aligning it against the provisional sync
    // pattern (bits), in [-max_row_shift, max_row_shift]; 0 for a row that needed none.
    std::vector<int> row_shift;

    std::vector<RasterColumn> columns;  // size F

    // The longest run of Constant columns starting at column 0.
    Bits sync_word;
    // The contiguous prefix of columns, starting at 0, that are not Payload (the sync
    // word plus any counter-like fields immediately following it): the header.
    std::vector<size_t> header_columns;
    // Every place sync_word recurs in the ORIGINAL bitstream (not just the fold),
    // within sync_max_bit_errors bits — this also catches frames the fold missed.
    std::vector<size_t> sync_positions;
};

struct RasterOptions {
    // A huge capture can fold into an enormous number of rows if the detected period turns
    // out small (millions of rows for a multi-megabit stream), and every stage after
    // folding costs at least one pass over every row -- unbounded, that stalls the UI on a
    // large file. Only the first `max_analysis_bits` bits are analysed; 0 means unbounded
    // (used by the tests, which never approach this in practice).
    size_t max_analysis_bits = 500'000;
    unsigned min_period = 4;
    // 0 means "auto": n / 3, capped at 2048, so at least 3 repeats are required.
    unsigned max_period = 0;
    // Candidate lengths to try before the general search (e.g. spikes from a rank
    // analysis); tried in the order given, first one that is a statistically real peak
    // (see build_frame_raster's implementation notes) wins.
    std::vector<unsigned> candidate_periods;
    // Row re-alignment searches per-row shifts in [-max_row_shift, max_row_shift]; 0
    // disables re-alignment.
    unsigned max_row_shift = 4;
    // s_j at or above this is labelled Constant.
    double constant_threshold = 0.9;
    // transition_rate within this of a dyadic value (1, 1/2, 1/4, ...) is CounterLike.
    double counter_tolerance = 0.12;
    // Bit-error tolerance when searching the whole stream for the recovered sync word.
    int sync_max_bit_errors = 1;
};

// Throws std::invalid_argument only for empty input; everything else that fails to
// find structure returns `detected == false` instead of throwing or guessing.
FrameRaster build_frame_raster(const Bits& bits, const RasterOptions& options = {});

}  // namespace sigapp
