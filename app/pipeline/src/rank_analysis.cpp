#include "rank_analysis.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>

namespace sigapp {

namespace {

// The expected rank deficiency (corank) of a random m x n matrix over GF(2), in the
// bit position convention used here (rows = codewords, columns = bit positions), once
// min(m, n) is not tiny: a well-known constant, independent of the larger dimension,
// approximately 0.7869 -- it is NOT zero, because random square GF(2) matrices are
// singular surprisingly often (about 71% of the time for a square matrix; corank 0, 1,
// 2, ... each have their own non-negligible probability, and this is their mean). A
// naive comparison against a floor of zero would flag almost every width as "deficient"
// purely from this baseline randomness; every width's excess here is measured against
// it instead.
constexpr double kExpectedRandomCorank = 0.7869;

double baseline_deficiency(size_t rows, unsigned width) {
    const double forced = rows < width ? static_cast<double>(width - rows) : 0.0;  // rank can never exceed rows
    return forced + kExpectedRandomCorank;
}

// Bit-packed GF(2) Gaussian elimination: rank of `rows` (each `words` 64-bit words,
// column j is bit j%64 of word j/64) over `width` columns.
unsigned gf2_rank(std::vector<std::vector<uint64_t>> rows, unsigned width) {
    const unsigned words = (width + 63) / 64;
    unsigned rank = 0;
    for (unsigned col = 0; col < width && rank < rows.size(); ++col) {
        const unsigned w = col / 64, b = col % 64;
        size_t pivot = rows.size();
        for (size_t r = rank; r < rows.size(); ++r) {
            if ((rows[r][w] >> b) & 1u) { pivot = r; break; }
        }
        if (pivot == rows.size()) continue;  // this column is already all-zero: deficient
        std::swap(rows[rank], rows[pivot]);
        for (size_t r = 0; r < rows.size(); ++r) {
            if (r != rank && ((rows[r][w] >> b) & 1u)) {
                for (unsigned k = 0; k < words; ++k) rows[r][k] ^= rows[rank][k];
            }
        }
        ++rank;
    }
    return rank;
}

// Folds bits[offset, offset + rows*width) into `rows` bit-packed rows of `width` bits.
std::vector<std::vector<uint64_t>> pack_rows(const Bits& bits, size_t offset, size_t rows, unsigned width) {
    const unsigned words = (width + 63) / 64;
    std::vector<std::vector<uint64_t>> packed(rows, std::vector<uint64_t>(words, 0));
    for (size_t r = 0; r < rows; ++r) {
        for (unsigned j = 0; j < width; ++j) {
            if (bits[offset + r * width + j]) packed[r][j / 64] |= (uint64_t{1} << (j % 64));
        }
    }
    return packed;
}

// The median rank across independent row-windows of `width` bits (see RankAnalysisOptions::
// window_rows): a window untouched by a bit error recovers the width's true rank exactly,
// and the median survives a minority of corrupted windows outvoting the rest, provided most
// windows stay clean -- see rank_analysis.hpp's "honest limits" for how few errors that
// tolerates in practice.
struct WidthMeasurement {
    double median_rank = 0.0;
    unsigned windows_used = 0;
};

WidthMeasurement measure_width(const Bits& bits, unsigned width, unsigned window_rows) {
    const size_t total_rows = bits.size() / width;
    const size_t rows_per_window = window_rows == 0 ? total_rows : std::min<size_t>(window_rows, total_rows);
    WidthMeasurement out;
    if (rows_per_window == 0) return out;

    std::vector<double> ranks;
    for (size_t start_row = 0; start_row + rows_per_window <= total_rows; start_row += rows_per_window) {
        const auto packed = pack_rows(bits, start_row * width, rows_per_window, width);
        ranks.push_back(static_cast<double>(gf2_rank(packed, width)));
    }
    if (ranks.empty()) return out;

    std::sort(ranks.begin(), ranks.end());
    const size_t mid = ranks.size() / 2;
    out.median_rank = (ranks.size() % 2 == 0) ? (ranks[mid - 1] + ranks[mid]) / 2.0 : ranks[mid];
    out.windows_used = static_cast<unsigned>(ranks.size());
    return out;
}

double value_at(const std::vector<RankProfilePoint>& profile, unsigned min_width, long width) {
    const long idx = width - static_cast<long>(min_width);
    return (idx >= 0 && static_cast<size_t>(idx) < profile.size()) ? profile[static_cast<size_t>(idx)].excess_deficiency
                                                                    : -1e9;  // out of range: never wins a comparison
}

// Same "smallest local maximum that clears an absolute floor, preferred candidates
// first, else the single strongest value in range" search as raster.cpp's
// pick_frame_length, for the same reason: a genuine but weaker fundamental spike must
// not be rejected just because some other width (very often an exact multiple of it --
// folding two codewords per row still lands in a proper subspace of the doubled space)
// happens to look stronger.
std::optional<unsigned> pick_width(const std::vector<RankProfilePoint>& profile, unsigned min_width,
                                   unsigned max_width, double threshold, const std::vector<unsigned>& preferred) {
    for (unsigned w : preferred) {
        if (w >= min_width && w <= max_width && value_at(profile, min_width, w) >= threshold) return w;
    }
    double best = -1e9;
    for (unsigned w = min_width; w <= max_width; ++w) best = std::max(best, value_at(profile, min_width, w));
    if (best < threshold) return std::nullopt;

    for (unsigned w = min_width; w <= max_width; ++w) {
        const double v = value_at(profile, min_width, w);
        if (v < threshold) continue;
        if (v >= value_at(profile, min_width, w - 1) && v >= value_at(profile, min_width, w + 1)) return w;
    }
    for (unsigned w = min_width; w <= max_width; ++w) {
        if (value_at(profile, min_width, w) == best) return w;
    }
    return std::nullopt;
}

}  // namespace

RankAnalysis build_rank_analysis(const Bits& bits_in, const RankAnalysisOptions& options) {
    if (bits_in.empty()) {
        throw std::invalid_argument("build_rank_analysis: bits is empty");
    }

    // See RankAnalysisOptions::max_analysis_bits: bound the work regardless of capture
    // size, exactly as raster.cpp does.
    Bits truncated_storage;
    if (options.max_analysis_bits > 0 && bits_in.size() > options.max_analysis_bits) {
        truncated_storage.assign(bits_in.begin(), bits_in.begin() + static_cast<long>(options.max_analysis_bits));
    }
    const Bits& bits = truncated_storage.empty() ? bits_in : truncated_storage;
    const size_t n = bits.size();

    RankAnalysis out;
    const unsigned min_width = std::max(2u, options.min_width);
    unsigned max_width =
        options.max_width != 0 ? options.max_width : static_cast<unsigned>(std::min<size_t>(512, n / 9));
    max_width = std::min<unsigned>(max_width, n > 0 ? static_cast<unsigned>(n - 1) : 0);
    if (max_width <= min_width || n < 9u * min_width) {
        return out;  // too little data for even the smallest candidate width
    }

    out.profile.reserve(max_width - min_width + 1);
    for (unsigned w = min_width; w <= max_width; ++w) {
        const WidthMeasurement m = measure_width(bits, w, options.window_rows);
        RankProfilePoint p;
        p.width = w;
        p.rank = m.median_rank;
        p.baseline_deficiency = baseline_deficiency(bits.size() / w, w);
        p.excess_deficiency = (static_cast<double>(w) - m.median_rank) - p.baseline_deficiency;
        out.profile.push_back(p);
    }

    const auto width = pick_width(out.profile, min_width, max_width, options.excess_deficiency_threshold,
                                   options.candidate_widths);
    if (!width.has_value()) return out;

    const RankProfilePoint& winner = out.profile[*width - min_width];
    out.width = *width;
    out.rank = winner.rank;
    out.code_rate = *width > 0 ? winner.rank / static_cast<double>(*width) : 0.0;
    out.windows_used = measure_width(bits, *width, options.window_rows).windows_used;
    out.detected = true;
    return out;
}

}  // namespace sigapp
