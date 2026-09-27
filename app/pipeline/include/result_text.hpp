#pragma once

// Port of the native GUI's text output (app/gui/results_panel.py and the main
// window's status line), kept free of Qt so it is unit-tested natively against
// the ORIGINAL panel's text (tests/data/pipeline/pipeline.rec, `panel_text_hex`):
// Python's `%g` number formatting, `repr()` of a str, and list printing are the
// awkward parts, and the output must match byte for byte.

#include <string>

#include "pipeline.hpp"
#include "raster.hpp"

namespace sigapp {

// Python's format(x, "g"): 6 significant digits, no trailing zeros (8000.0 -> "8000",
// 1e6 -> "1e+06").
std::string format_g(double value);

// Python's repr() of a str made of ASCII: single-quoted, unless it contains a
// ' and no ", with \\ \n \r \t and other control characters escaped.
std::string python_repr(const std::string& text);

// The results panel's text for a finished analysis. `max_message_chars` limits how
// much of the decoded message is printed: a longer one shows its first
// `max_message_chars` characters followed by a note of the total (the default, no
// limit, is what the original prints). A capture of millions of bits decodes to a
// message of megabytes, and a single line that long makes Qt's text layout take
// minutes, so the GUI passes a limit (see ResultsPanel).
// `show_soft_decoding_info`: appends two lines the ORIGINAL app never had (Feature 3,
// SIH26147 extra feature) -- "Decoding mode: hard/soft" and, whenever a FEC scheme was
// used, "Re-encode agreement: 0.xx" (Section 2.3 of the feature spec: the decoded
// message re-encoded and compared against what was actually received, the same score
// in both modes). Defaults to false so this function's output for a plain analysis
// stays byte-identical to the original GUI panel's text (checked in test_result_text.cpp
// against 70 recorded scenarios); the GUI's ResultsPanel passes true.
std::string format_result_text(const PipelineResult& result, size_t max_message_chars = SIZE_MAX,
                               bool show_soft_decoding_info = false);

// The status bar line after a successful run:
// "Done. Predicted modulation: bpsk (100% confidence)."
std::string format_done_status(const PipelineResult& result);

// Feature 4 (SIH26147 extra feature) panel text: frame length, sync word (bits and hex),
// header composition and column labels, or a plain "nothing found" line when
// raster.detected is false. New to this port -- there is no original-app text to match.
std::string format_raster_text(const FrameRaster& raster);

}  // namespace sigapp
