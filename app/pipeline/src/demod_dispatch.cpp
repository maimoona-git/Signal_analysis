#include "demod_dispatch.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <stdexcept>

namespace sigapp {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}  // namespace

sigcore::ModulationScheme linear_scheme_from_name(const std::string& name) {
    using S = sigcore::ModulationScheme;
    static const std::map<std::string, S> kSchemes = {
        {"bpsk", S::BPSK},     {"qpsk", S::QPSK},     {"psk8", S::PSK8},     {"psk16", S::PSK16},
        {"psk32", S::PSK32},   {"psk64", S::PSK64},   {"qam4", S::QAM4},     {"qam16", S::QAM16},
        {"qam32", S::QAM32},   {"qam64", S::QAM64},   {"qam128", S::QAM128}, {"qam256", S::QAM256},
        {"ask2", S::ASK2},     {"ask4", S::ASK4},     {"ask8", S::ASK8},     {"ask16", S::ASK16},
        {"ook", S::OOK},
    };
    const auto it = kSchemes.find(lower(name));
    if (it == kSchemes.end()) {
        throw std::invalid_argument("unknown modulation scheme: '" + name + "'");
    }
    return it->second;
}

std::vector<std::complex<float>> modulate(const Bits& bits, const std::string& scheme,
                                          const DemodParams& params) {
    if (lower(scheme) == "fsk") {
        return sigcore::fsk_modulate(bits, params.bits_per_symbol, params.samples_per_symbol, params.bandwidth);
    }
    return sigcore::linear_modulate(bits, linear_scheme_from_name(scheme));
}

Bits demodulate(const std::vector<std::complex<float>>& samples, const std::string& scheme,
                const DemodParams& params) {
    if (lower(scheme) == "fsk") {
        return sigcore::fsk_demodulate(samples, params.bits_per_symbol, params.samples_per_symbol, params.bandwidth);
    }
    return sigcore::linear_demodulate(samples, linear_scheme_from_name(scheme));
}

}  // namespace sigapp
