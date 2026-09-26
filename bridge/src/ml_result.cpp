#include "ml_result.hpp"

#include <cmath>
#include <stdexcept>

namespace sigbridge {

MlResult make_ml_result(std::vector<std::string> classes, std::vector<float> probabilities) {
    if (classes.empty()) {
        throw std::runtime_error("ml result: no classes");
    }
    if (classes.size() != probabilities.size()) {
        throw std::runtime_error("ml result: " + std::to_string(classes.size()) + " classes but " +
                                 std::to_string(probabilities.size()) + " probabilities");
    }
    std::size_t top = 0;
    for (std::size_t i = 0; i < classes.size(); ++i) {
        if (classes[i].empty()) {
            throw std::runtime_error("ml result: empty class name at index " + std::to_string(i));
        }
        const float p = probabilities[i];
        if (!std::isfinite(p) || p < 0.0f || p > 1.0f) {
            throw std::runtime_error("ml result: probability " + std::to_string(i) +
                                     " is not a finite number in [0, 1]");
        }
        if (p > probabilities[top]) {
            top = i;
        }
    }

    MlResult r;
    r.top_index = top;
    r.label = classes[top];
    r.confidence = probabilities[top];
    r.classes = std::move(classes);
    r.probabilities = std::move(probabilities);
    return r;
}

}  // namespace sigbridge
