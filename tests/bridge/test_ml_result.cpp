// Phase 4: validation of what the JS classifier returns. Portable C++, run
// natively and under WASM. Nothing from the JS side is trusted.

#include <cmath>
#include <limits>
#include <string>
#include <vector>

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

TEST_MAIN()
