#pragma once

#include <complex>
#include <string>
#include <vector>

namespace sigcore {

struct SignalData {
    std::vector<std::complex<float>> samples;
    double sample_rate = 0.0;
    std::string format;

    size_t num_samples() const { return samples.size(); }
    double duration_sec() const {
        return sample_rate > 0.0 ? static_cast<double>(samples.size()) / sample_rate : 0.0;
    }
};

}  // namespace sigcore
