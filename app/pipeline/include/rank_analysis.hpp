#pragma once

// Feature 2 (rank-analysis view, SIH26147 extra feature): a BLIND way to find a linear
// block code's length (and from it, its rate) without decoding anything. Fold the
// bitstream into an R x W matrix (rows = W-bit blocks) for a range of candidate widths
// W, and compute its rank over GF(2). A width that is NOT a real code/interleaver
// boundary folds essentially random bits, which is (perhaps surprisingly) very rarely
// full rank on its own -- see build_rank_analysis's implementation notes -- but a width
// that IS the true codeword length folds every row into the same k-dimensional code
// subspace, so the rank saturates at k long before W rows have been seen: an excess
// rank DEFICIT that stands out above the random baseline. Where it appears is the code
// length n = W; the rank itself there estimates the code dimension k, so k/n estimates
// the code rate. No knowledge of the actual FEC scheme is needed for any of this.
//
// This runs on the bits BEFORE any decode attempt (demodulated_bits): decoding a code
// removes its own redundancy, which is exactly the signal this looks for -- see
// pipeline.cpp for where it is actually run.
//
// Honest limits (measured, not assumed -- see rank_analysis.cpp and app/pipeline/README.md):
//  - Bit errors are much more damaging here than to Feature 4's frame raster: a single
//    flipped bit in a row can, on its own, add one full new independent dimension. A
//    code with few redundant bits (small n - k) has correspondingly little room to
//    absorb that before the deficiency signal disappears entirely.
//  - No offset/phase search: folding always starts at bit 0, the same assumption the
//    rest of the pipeline makes (find_header_payload_boundaries looks for its header
//    from the start of the stream too). A capture whose first codeword does not start
//    at bit 0 will not show a clean spike.
//
// Pure C++, no liquid-dsp: builds and runs identically natively and on WASM.

#include <cstddef>
#include <vector>

#include "bits.hpp"

namespace sigapp {

struct RankProfilePoint {
    unsigned width = 0;
    double rank = 0.0;               // median rank across windows at this width
    double baseline_deficiency = 0.0;  // the expected deficiency of a same-shaped RANDOM matrix
    double excess_deficiency = 0.0;    // (width - rank) - baseline_deficiency; the actual signal
};

struct RankAnalysis {
    // False when no width's excess deficiency clears the detection floor: everything
    // else is left at its default, rather than reporting a low-confidence guess as real.
    bool detected = false;
    unsigned width = 0;          // the detected codeword length n
    double rank = 0.0;           // estimated dimension k (median across windows)
    double code_rate = 0.0;      // rank / width
    unsigned windows_used = 0;   // how many independent row-windows contributed the median

    // The full sweep, one entry per width tried, in width order -- what a "rank profile"
    // chart plots (a spike at `width`, if detected).
    std::vector<RankProfilePoint> profile;
};

struct RankAnalysisOptions {
    // See raster.hpp's identical option for the same reason: bound the work regardless
    // of capture size.
    size_t max_analysis_bits = 500'000;
    unsigned min_width = 2;
    // 0 means "auto": min(512, n / 9), so even the widest candidate gets at least 3
    // windows of 3 codewords each.
    unsigned max_width = 0;
    // Widths to try before the general sweep (e.g. a period Feature 4's raster already
    // found); tried in the order given, first one whose excess deficiency clears the
    // floor wins, same convention as RasterOptions::candidate_periods.
    std::vector<unsigned> candidate_widths;
    // Rows per window for the median-of-windows computation (see the header comment's
    // "honest limits"): a window with no bit errors in it recovers the true rank exactly;
    // taking the MEDIAN across many windows survives a minority of corrupted windows
    // outvoting the rest, provided most windows stay clean. 0 means "one window, all
    // available rows" -- the most sensitive setting, and the right default for a clean
    // or synthetic capture, but with no error tolerance at all (one bad row anywhere
    // corrupts the only window there is).
    unsigned window_rows = 0;
    // An excess deficiency (over the random-matrix baseline) at or above this, in bits,
    // is a real spike. The random baseline's own spread is small and its tail decays
    // very fast (unlike Feature 4's autocorrelation, this does not need to scale with
    // capture size -- see build_rank_analysis's implementation notes), so a small
    // absolute floor is already a safe one.
    double excess_deficiency_threshold = 3.0;
};

// Throws std::invalid_argument only for empty input; everything else that fails to
// find a spike returns `detected == false` instead of guessing.
RankAnalysis build_rank_analysis(const Bits& bits, const RankAnalysisOptions& options = {});

}  // namespace sigapp
