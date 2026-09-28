#pragma once

// Turns a caught exception into the message a user should see. Portable C++ (no
// Emscripten), so it is unit-tested natively.

#include <exception>
#include <new>
#include <string>

namespace sigbridge {

namespace detail {

inline bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace detail

// std::bad_alloc's what() is "std::bad_alloc": true but useless to someone who just
// dropped a very large capture on the page. A few other internal preconditions are
// real, common ways for an ordinary (non-synthetic) file to fail -- e.g. the shipped
// ML classifier is only trained on this project's own synthetic captures (a known,
// documented limitation: see ml/README.md), so it can and does mis-classify a real
// downloaded recording as some scheme the file was never actually modulated with; the
// clearest symptom is core/src/demod.cpp's FSK path throwing over a sample count that
// happens not to be a whole number of symbols. Found via a real user report: the raw
// message ("fsk_demodulate: samples length must be a multiple of sps") reached the
// results panel and a dialog box verbatim, developer jargon and all. Everything else
// keeps its own message.
inline std::string describe_exception(const std::exception& e) {
    if (dynamic_cast<const std::bad_alloc*>(&e) != nullptr) {
        return "out of memory: this capture is too large for the browser to hold while analysing it. "
               "Try a shorter capture.";
    }

    const std::string what = e.what();
    if (detail::contains(what, "fsk_demodulate: samples length must be a multiple of sps")) {
        return "This capture doesn't demodulate cleanly as FSK: its length isn't a whole number of "
               "symbols. The automatic modulation guess is likely wrong for this file -- this is "
               "expected for a real-world recording that wasn't built for this tool (the classifier "
               "only recognises the modulation schemes it was trained on). Try a capture made for "
               "this pipeline instead.";
    }
    if (detail::contains(what, "not aligned to complex64 sample size")) {
        return "This .IQ file's size isn't a whole number of I/Q sample pairs, so it can't be a raw "
               "complex64 capture. Check that it's actually raw I/Q data, not a compressed or "
               "otherwise different file format.";
    }
    if (detail::contains(what, "data size not aligned to frame size")) {
        return "This WAV file's audio data isn't a whole number of sample frames, so it may be "
               "corrupted, or use a format this tool doesn't support (compressed audio, an unusual "
               "bit depth, and similar).";
    }
    return what;
}

}  // namespace sigbridge
