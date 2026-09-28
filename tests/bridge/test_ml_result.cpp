// Phase 4: validation of what the JS classifier returns. Portable C++, run
// natively and under WASM. Nothing from the JS side is trusted.

#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#include "error_text.hpp"
#include "ml_result.hpp"
#include "../harness.hpp"

using namespace sigbridge;

TEST_CASE("make_ml_result picks the top class") {
    const auto r = make_ml_result({"BPSK", "QPSK", "FSK"}, {0.1f, 0.7f, 0.2f});
    CHECK(r.label == "QPSK");
    CHECK(r.top_index == 1);
    CHECK_NEAR(r.confidence, 0.7, 1e-6);
    CHECK(r.classes.size() == 3 && r.probabilities.size() == 3);
}

TEST_CASE("ties go to the first class; a single class is fine") {
    CHECK(make_ml_result({"a", "b", "c"}, {0.5f, 0.5f, 0.0f}).label == "a");
    const auto one = make_ml_result({"only"}, {1.0f});
    CHECK(one.label == "only" && one.top_index == 0);
}

TEST_CASE("boundary probabilities 0 and 1 are accepted") {
    CHECK(make_ml_result({"a", "b"}, {0.0f, 1.0f}).label == "b");
}

TEST_CASE("make_ml_result rejects malformed classifier output") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    CHECK_THROWS(make_ml_result({}, {}));                          // nothing
    CHECK_THROWS(make_ml_result({"a", "b"}, {1.0f}));              // length mismatch
    CHECK_THROWS(make_ml_result({"a"}, {0.5f, 0.5f}));
    CHECK_THROWS(make_ml_result({"a", ""}, {0.5f, 0.5f}));         // empty class name
    CHECK_THROWS(make_ml_result({"a", "b"}, {nan, 0.5f}));         // NaN
    CHECK_THROWS(make_ml_result({"a", "b"}, {0.5f, inf}));         // inf
    CHECK_THROWS(make_ml_result({"a", "b"}, {-0.1f, 0.5f}));       // negative
    CHECK_THROWS(make_ml_result({"a", "b"}, {0.5f, 1.5f}));        // > 1 (e.g. raw logits)
}

TEST_CASE("describe_exception turns out-of-memory into an actionable message and leaves the rest alone") {
    const std::string oom = sigbridge::describe_exception(std::bad_alloc());
    CHECK(oom.find("out of memory") != std::string::npos);
    CHECK(oom.find("too large") != std::string::npos);
    CHECK(oom.find("bad_alloc") == std::string::npos);             // no C++ jargon shown to the user
    CHECK(sigbridge::describe_exception(std::runtime_error("wav: not a valid RIFF/WAVE file: x.wav")) ==
          "wav: not a valid RIFF/WAVE file: x.wav");
    CHECK(sigbridge::describe_exception(std::invalid_argument("bad option")) == "bad option");
    try { throw std::bad_alloc(); } catch (const std::exception& e) {   // as caught through the base class
        CHECK(sigbridge::describe_exception(e).find("out of memory") != std::string::npos);
    }
}

TEST_CASE("describe_exception explains a misclassified real-world capture in plain language") {
    // Found via a real user report: the classifier guessed FSK for an ordinary
    // downloaded WAV file, and the raw internal precondition message ("fsk_demodulate:
    // samples length must be a multiple of sps") reached the results panel verbatim.
    const std::string fsk = sigbridge::describe_exception(
        std::runtime_error("fsk_demodulate: samples length must be a multiple of sps"));
    CHECK(fsk.find("fsk_demodulate") == std::string::npos);   // no function/variable names shown to the user
    CHECK(fsk.find("sps") == std::string::npos);
    CHECK(fsk.find("FSK") != std::string::npos);              // still says which scheme, in plain terms
    CHECK(fsk.find("classifier") != std::string::npos);       // and why: a wrong automatic guess

    const std::string iq = sigbridge::describe_exception(
        std::runtime_error("iq: file size not aligned to complex64 sample size: x.iq"));
    CHECK(iq.find("x.iq") == std::string::npos);      // the raw message (with the bare filename) is gone
    CHECK(iq.find(".IQ file") != std::string::npos);  // still says which file type, in plain terms

    const std::string wav = sigbridge::describe_exception(
        std::runtime_error("wav: data size not aligned to frame size: x.wav"));
    CHECK(wav.find("WAV file") != std::string::npos);

    // A message that merely happens to share a word (e.g. another function's own
    // "sps" or "frame size" wording) is not rewritten -- only these three exact,
    // known internal messages are recognised.
    CHECK(sigbridge::describe_exception(std::runtime_error("some other error entirely")) ==
          "some other error entirely");
}

TEST_MAIN()
