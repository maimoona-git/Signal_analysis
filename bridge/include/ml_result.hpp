#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace sigbridge {

// What the JS-side classifier hands back: one probability per class.
// Portable C++ (no Emscripten) so the validation runs in the native tests too.
struct MlResult {
    std::string label;                  // classes[top_index]
    float confidence = 0.0f;            // probabilities[top_index]
    std::size_t top_index = 0;
    std::vector<std::string> classes;
    std::vector<float> probabilities;   // same length as classes, each in [0, 1]
};

// Validates a classifier's raw output and picks the top class (first one wins
// on ties). Throws std::runtime_error if it is empty, the lengths differ, a
// class name is empty, or a probability is NaN/inf or outside [0, 1]. Nothing
// from the JS side is trusted: a buggy model must not become a wrong answer.
MlResult make_ml_result(std::vector<std::string> classes, std::vector<float> probabilities);

}  // namespace sigbridge
