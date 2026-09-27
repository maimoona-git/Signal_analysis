#include "raster.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

#include "correlate.hpp"

namespace sigapp {

namespace {

// Reads corr[lag - 1] (autocorrelation is defined for lags 1..bits.size()-1); a
// lag outside that range reads as an impossible sentinel so it never wins a comparison.
double value_at(const std::vector<double>& corr, long lag) {
    return (lag >= 1 && static_cast<size_t>(lag) <= corr.size()) ? corr[static_cast<size_t>(lag) - 1] : -2.0;
}

// A noise floor a peak must clear in absolute terms (not just relative to the
// best-in-range), or nothing is reported. This has to scale with the stream length:
// a random N-bit stream's autocorrelation at any one lag has standard deviation
// ~1/sqrt(N), so a fixed constant is far too permissive on a short capture (measured:
// a single 64-bit ASCII message spuriously "detected" an 8-bit period from nothing
// but the text's own incidental redundancy, comfortably clearing a flat 0.15). Five
// standard deviations keeps the false-positive rate low even when many candidate
// lags are being scanned at once.
double min_peak_value(size_t n) {
    return std::max(0.05, 5.0 / std::sqrt(static_cast<double>(std::max<size_t>(n, 1))));
}

// The smallest lag with a statistically real autocorrelation peak in
// [min_period, max_period]: the fundamental period, not one of its harmonics (which
// also correlate strongly).
//
// A peak only has to clear the absolute noise floor, not some fraction of the
// strongest peak found anywhere in the range. That second, relative test looks like a
// reasonable extra safeguard, but it isn't: a header field that changes every frame
// (a frame counter's own LSB) actively *decorrelates* the stream at exactly the true
// period (adjacent frames are guaranteed to differ there) while *correlating* at even
// multiples of it (two frames on, the LSB has flipped twice and matches again) --
// measured on a synthetic counter-bearing stream, this can make a harmonic (e.g. 2x
// or 4x the real length) genuinely stronger than the fundamental. Requiring the
// fundamental to match a fraction of that inflated harmonic then rejects the very
// peak this function exists to find. The absolute floor already keeps this from
// picking up noise; a real signal's own weaker fundamental deserves to still win it.
std::optional<unsigned> pick_frame_length(const std::vector<double>& corr, unsigned min_period,
                                          unsigned max_period, const std::vector<unsigned>& preferred) {
    const double min_peak = min_peak_value(corr.size() + 1);  // corr has n - 1 entries
    double best_in_range = -2.0;
    for (unsigned lag = min_period; lag <= max_period; ++lag) best_in_range = std::max(best_in_range, value_at(corr, lag));
    if (best_in_range < min_peak) return std::nullopt;  // nothing periodic here

    for (unsigned lag : preferred) {
        if (lag >= min_period && lag <= max_period && value_at(corr, lag) >= min_peak) return lag;
    }
    for (unsigned lag = min_period; lag <= max_period; ++lag) {
        const double v = value_at(corr, lag);
        if (v < min_peak) continue;
        if (v >= value_at(corr, lag - 1) && v >= value_at(corr, lag + 1)) return lag;  // a local maximum
    }
    // No clean local maximum cleared the floor (e.g. very few repeats): fall back to
    // the single strongest lag in range.
    for (unsigned lag = min_period; lag <= max_period; ++lag) {
        if (value_at(corr, lag) == best_in_range) return lag;
    }
    return std::nullopt;
}

// Labels the counter field, if any, right after the leading Constant run. A single
// column's transition rate near 0.5 is exactly what independent random payload looks
// like too (that IS its expected rate), so it can't be used alone to spot a counter bit.
// What genuinely distinguishes a counter is the whole *sequence*: the field's own LSB
// flips almost every frame (rate near 1.0, which random data essentially never does),
// and each bit after it flips at half the rate of the one before. So the run is anchored
// on that first, discriminating column, then extended only while the halving prediction
// keeps holding — which real payload quickly fails, since its rate stays near 0.5
// regardless of how far the expected value has shrunk.
void label_counter_run(std::vector<RasterColumn>& columns, double tolerance) {
    size_t j = 0;
    while (j < columns.size() && columns[j].label == ColumnLabel::Constant) ++j;

    // expected starts at 1.0, so the very first candidate is only accepted (the loop only
    // runs at all) if IT looks like a counter's LSB -- the anchor requirement isn't a
    // separate check, it's just this loop's own first iteration.
    double expected = 1.0;
    while (j < columns.size() && columns[j].label != ColumnLabel::Constant &&
          std::fabs(columns[j].transition_rate - expected) <= tolerance) {
        columns[j].label = ColumnLabel::CounterLike;
        expected /= 2.0;
        ++j;
    }
}

}  // namespace

FrameRaster build_frame_raster(const Bits& bits_in, const RasterOptions& options) {
    if (bits_in.empty()) {
        throw std::invalid_argument("build_frame_raster: bits is empty");
    }

    // See RasterOptions::max_analysis_bits: bound the work regardless of how large the
    // capture is. `bits` refers to `bits_in` directly (no copy) unless truncation is
    // actually needed.
    Bits truncated_storage;
    if (options.max_analysis_bits > 0 && bits_in.size() > options.max_analysis_bits) {
        truncated_storage.assign(bits_in.begin(), bits_in.begin() + static_cast<long>(options.max_analysis_bits));
    }
    const Bits& bits = truncated_storage.empty() ? bits_in : truncated_storage;

    FrameRaster out;
    const size_t n = bits.size();

    const unsigned min_period = std::max(2u, options.min_period);
    unsigned max_period =
        options.max_period != 0 ? options.max_period : static_cast<unsigned>(std::min<size_t>(2048, n / 3));
    max_period = std::min<unsigned>(max_period, n > 0 ? static_cast<unsigned>(n - 1) : 0);
    if (max_period <= min_period || n < 3u * min_period) {
        return out;  // too little data for even one candidate repeat
    }

    const auto corr = autocorrelation(bits);  // lags 1..n-1
    const auto period = pick_frame_length(corr, min_period, max_period, options.candidate_periods);
    if (!period.has_value()) return out;

    const unsigned f = *period;
    const unsigned rows = static_cast<unsigned>(n / f);
    if (rows < 2) return out;

    out.frame_length = f;
    out.num_frames = rows;

    // Row r's bits are [r*f + shift[r], r*f + shift[r] + f); a shift that would run off
    // either end of the stream is clamped back to 0 for that row (always in range, since
    // r < rows means r*f + f - 1 <= rows*f - 1 <= n - 1).
    auto fold = [&](const std::vector<int>& shift) {
        Bits m(static_cast<size_t>(rows) * f);
        for (unsigned r = 0; r < rows; ++r) {
            for (unsigned j = 0; j < f; ++j) {
                long idx = static_cast<long>(r) * f + j + shift[r];
                if (idx < 0 || idx >= static_cast<long>(n)) idx = static_cast<long>(r) * f + j;
                m[static_cast<size_t>(r) * f + j] = bits[static_cast<size_t>(idx)];
            }
        }
        return m;
    };
    auto column_ones_fraction = [&](const Bits& m, unsigned j) {
        size_t ones = 0;
        for (unsigned r = 0; r < rows; ++r) ones += m[static_cast<size_t>(r) * f + j];
        return static_cast<double>(ones) / rows;
    };

    const std::vector<int> no_shift(rows, 0);
    const Bits first_pass = fold(no_shift);

    // Provisional sync pattern: the columns the unaligned first pass finds MOST stable
    // (ranked, not a fixed threshold). A truly constant column's stability dilutes to
    // (1 - misaligned_fraction) once a slip corrupts some rows, but with only a handful
    // of corrupted rows contributing "noise", the exact dilution any one column shows has
    // real sampling variance -- a fixed cutoff can arbitrarily exclude a column that is
    // still the most informative one available. Ranking and taking the top few is robust
    // to that: it always returns a usable reference set as long as anything is more
    // stable than pure noise, rather than requiring every column to individually clear an
    // absolute bar.
    std::vector<unsigned> ranked(f);
    std::iota(ranked.begin(), ranked.end(), 0u);
    std::sort(ranked.begin(), ranked.end(), [&](unsigned a, unsigned b) {
        return std::fabs(2.0 * column_ones_fraction(first_pass, a) - 1.0) >
               std::fabs(2.0 * column_ones_fraction(first_pass, b) - 1.0);
    });
    std::vector<unsigned> provisional_columns;
    const unsigned max_provisional = std::max(4u, f / 4);
    for (unsigned col : ranked) {
        if (provisional_columns.size() >= max_provisional) break;
        if (std::fabs(2.0 * column_ones_fraction(first_pass, col) - 1.0) < 0.15) break;  // nothing left worth using
        provisional_columns.push_back(col);
    }
    std::sort(provisional_columns.begin(), provisional_columns.end());

    std::vector<int> row_shift(rows, 0);
    if (!provisional_columns.empty() && options.max_row_shift > 0) {
        std::vector<uint8_t> reference(provisional_columns.size());
        for (size_t k = 0; k < provisional_columns.size(); ++k) {
            reference[k] = column_ones_fraction(first_pass, provisional_columns[k]) >= 0.5 ? 1 : 0;
        }
        // Try shift 0 first, then +-1, +-2, ...: on a tie, the smallest correction wins.
        std::vector<int> shift_order = {0};
        for (int k = 1; k <= static_cast<int>(options.max_row_shift); ++k) { shift_order.push_back(k); shift_order.push_back(-k); }

        for (unsigned r = 0; r < rows; ++r) {
            int best_shift = 0, best_matches = -1;
            for (int d : shift_order) {
                int matches = 0;
                bool in_bounds = true;
                for (size_t k = 0; k < provisional_columns.size(); ++k) {
                    const long idx = static_cast<long>(r) * f + provisional_columns[k] + d;
                    if (idx < 0 || idx >= static_cast<long>(n)) { in_bounds = false; break; }
                    matches += (bits[static_cast<size_t>(idx)] == reference[k]);
                }
                if (in_bounds && matches > best_matches) { best_matches = matches; best_shift = d; }
            }
            row_shift[r] = best_shift;
        }
    }
    out.row_shift = row_shift;
    out.matrix = fold(row_shift);

    out.columns.resize(f);
    for (unsigned j = 0; j < f; ++j) {
        const double p = column_ones_fraction(out.matrix, j);
        unsigned transitions = 0;
        for (unsigned r = 0; r + 1 < rows; ++r) {
            transitions += (out.matrix[r * f + j] != out.matrix[(r + 1) * f + j]);
        }
        RasterColumn col;
        col.ones_fraction = p;
        col.stability = std::fabs(2.0 * p - 1.0);
        col.transition_rate = rows > 1 ? static_cast<double>(transitions) / (rows - 1) : 0.0;
        col.label = col.stability >= options.constant_threshold ? ColumnLabel::Constant : ColumnLabel::Payload;
        out.columns[j] = col;
    }
    label_counter_run(out.columns, options.counter_tolerance);

    unsigned sync_len = 0;
    while (sync_len < f && out.columns[sync_len].label == ColumnLabel::Constant) ++sync_len;
    out.sync_word.resize(sync_len);
    for (unsigned j = 0; j < sync_len; ++j) out.sync_word[j] = out.columns[j].ones_fraction >= 0.5 ? 1 : 0;

    unsigned header_len = 0;
    while (header_len < f && out.columns[header_len].label != ColumnLabel::Payload) ++header_len;
    out.header_columns.resize(header_len);
    for (unsigned j = 0; j < header_len; ++j) out.header_columns[j] = j;

    if (!out.sync_word.empty()) {
        out.sync_positions = find_pattern_occurrences(bits, out.sync_word, options.sync_max_bit_errors);
    }

    out.detected = true;
    return out;
}

}  // namespace sigapp
