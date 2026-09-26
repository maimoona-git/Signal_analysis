#pragma once

// Port of the native GUI's text output (app/gui/results_panel.py and the main
// window's status line), kept free of Qt so it is unit-tested natively against
// the ORIGINAL panel's text (tests/data/pipeline/pipeline.rec, `panel_text_hex`):
// Python's `%g` number formatting, `repr()` of a str, and list printing are the
// awkward parts, and the output must match byte for byte.

#include <string>

#include "pipeline.hpp"

namespace sigapp {

// Python's format(x, "g"): 6 significant digits, no trailing zeros (8000.0 -> "8000",
// 1e6 -> "1e+06").
std::string format_g(double value);

// Python's repr() of a str made of ASCII: single-quoted, unless it contains a
// ' and no ", with \\ \n \r \t and other control characters escaped.
std::string python_repr(const std::string& text);

// The results panel's text for a finished analysis.
std::string format_result_text(const PipelineResult& result);

// The status bar line after a successful run:
// "Done. Predicted modulation: bpsk (100% confidence)."
std::string format_done_status(const PipelineResult& result);

}  // namespace sigapp
