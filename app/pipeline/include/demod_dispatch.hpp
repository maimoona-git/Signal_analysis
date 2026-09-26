#pragma once

// Port of app/demod.py: modulation/demodulation by scheme *name*.

#include <complex>
#include <string>
#include <vector>

#include "bits.hpp"
#include "demod.hpp"

namespace sigapp {

constexpr float kDefaultFskBandwidth = 0.25f;
constexpr unsigned int kDefaultFskSamplesPerSymbol = 8;

// Only used for "fsk"; ignored for the linear schemes.
struct DemodParams {
    unsigned int bits_per_symbol = 1;
    unsigned int samples_per_symbol = kDefaultFskSamplesPerSymbol;
    float bandwidth = kDefaultFskBandwidth;
};

// bpsk qpsk psk8 psk16 psk32 psk64 qam4 qam16 qam32 qam64 qam128 qam256 ask2
// ask4 ask8 ask16 ook (case-insensitive). Throws std::invalid_argument
// otherwise. "fsk" is handled separately by modulate/demodulate.
sigcore::ModulationScheme linear_scheme_from_name(const std::string& name);

std::vector<std::complex<float>> modulate(const Bits& bits, const std::string& scheme,
                                          const DemodParams& params = {});
Bits demodulate(const std::vector<std::complex<float>>& samples, const std::string& scheme,
                const DemodParams& params = {});

}  // namespace sigapp
