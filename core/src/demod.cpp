#include "demod.hpp"

#include <complex>

#include <liquid/liquid.h>

#include <stdexcept>

namespace sigcore {

namespace {

liquid_float_complex* as_liquid(std::complex<float>* p) {
    return reinterpret_cast<liquid_float_complex*>(p);
}

modulation_scheme to_liquid_scheme(ModulationScheme scheme) {
    switch (scheme) {
        case ModulationScheme::BPSK: return LIQUID_MODEM_BPSK;
        case ModulationScheme::QPSK: return LIQUID_MODEM_QPSK;
        case ModulationScheme::PSK8: return LIQUID_MODEM_PSK8;
        case ModulationScheme::PSK16: return LIQUID_MODEM_PSK16;
        case ModulationScheme::PSK32: return LIQUID_MODEM_PSK32;
        case ModulationScheme::PSK64: return LIQUID_MODEM_PSK64;
        case ModulationScheme::QAM4: return LIQUID_MODEM_QAM4;
        case ModulationScheme::QAM16: return LIQUID_MODEM_QAM16;
        case ModulationScheme::QAM32: return LIQUID_MODEM_QAM32;
        case ModulationScheme::QAM64: return LIQUID_MODEM_QAM64;
        case ModulationScheme::QAM128: return LIQUID_MODEM_QAM128;
        case ModulationScheme::QAM256: return LIQUID_MODEM_QAM256;
        case ModulationScheme::ASK2: return LIQUID_MODEM_ASK2;
        case ModulationScheme::ASK4: return LIQUID_MODEM_ASK4;
        case ModulationScheme::ASK8: return LIQUID_MODEM_ASK8;
        case ModulationScheme::ASK16: return LIQUID_MODEM_ASK16;
        case ModulationScheme::OOK: return LIQUID_MODEM_OOK;
    }
    throw std::runtime_error("unknown modulation scheme");
}

}  // namespace

std::vector<unsigned int> bits_to_symbols(const std::vector<uint8_t>& bits, unsigned int bps) {
    if (bps == 0) {
        throw std::runtime_error("bits_to_symbols: bps must be > 0");
    }
    const size_t num_symbols = (bits.size() + bps - 1) / bps;
    std::vector<unsigned int> symbols(num_symbols, 0);

    for (size_t i = 0; i < bits.size(); ++i) {
        const size_t sym_idx = i / bps;
        const unsigned int bit_pos_in_symbol = bps - 1 - static_cast<unsigned int>(i % bps);
        if (bits[i] & 0x1) {
            symbols[sym_idx] |= (1u << bit_pos_in_symbol);
        }
    }
    return symbols;
}

std::vector<uint8_t> symbols_to_bits(const std::vector<unsigned int>& symbols, unsigned int bps) {
    if (bps == 0) {
        throw std::runtime_error("symbols_to_bits: bps must be > 0");
    }
    std::vector<uint8_t> bits(symbols.size() * bps);
    for (size_t i = 0; i < symbols.size(); ++i) {
        for (unsigned int b = 0; b < bps; ++b) {
            const unsigned int bit_pos_in_symbol = bps - 1 - b;
            bits[i * bps + b] = (symbols[i] >> bit_pos_in_symbol) & 0x1;
        }
    }
    return bits;
}

std::vector<std::complex<float>> linear_modulate(const std::vector<uint8_t>& bits,
                                                  ModulationScheme scheme) {
    if (bits.empty()) {
        throw std::runtime_error("linear_modulate: bits is empty");
    }
    modemcf mod = modemcf_create(to_liquid_scheme(scheme));
    const unsigned int bps = modemcf_get_bps(mod);

    auto symbols = bits_to_symbols(bits, bps);
    std::vector<std::complex<float>> out(symbols.size());
    for (size_t i = 0; i < symbols.size(); ++i) {
        modemcf_modulate(mod, symbols[i], as_liquid(&out[i]));
    }

    modemcf_destroy(mod);
    return out;
}

std::vector<uint8_t> linear_demodulate(const std::vector<std::complex<float>>& samples,
                                        ModulationScheme scheme) {
    if (samples.empty()) {
        throw std::runtime_error("linear_demodulate: samples is empty");
    }
    modemcf mod = modemcf_create(to_liquid_scheme(scheme));
    const unsigned int bps = modemcf_get_bps(mod);

    std::vector<unsigned int> symbols(samples.size());
    for (size_t i = 0; i < samples.size(); ++i) {
        liquid_float_complex x = *as_liquid(const_cast<std::complex<float>*>(&samples[i]));
        modemcf_demodulate(mod, x, &symbols[i]);
    }

    modemcf_destroy(mod);
    return symbols_to_bits(symbols, bps);
}

std::vector<std::complex<float>> fsk_modulate(const std::vector<uint8_t>& bits, unsigned int bps,
                                               unsigned int sps, float bandwidth) {
    if (bits.empty()) {
        throw std::runtime_error("fsk_modulate: bits is empty");
    }
    fskmod mod = fskmod_create(bps, sps, bandwidth);

    auto symbols = bits_to_symbols(bits, bps);
    std::vector<std::complex<float>> out(symbols.size() * sps);
    for (size_t i = 0; i < symbols.size(); ++i) {
        fskmod_modulate(mod, symbols[i], as_liquid(&out[i * sps]));
    }

    fskmod_destroy(mod);
    return out;
}

std::vector<uint8_t> fsk_demodulate(const std::vector<std::complex<float>>& samples,
                                     unsigned int bps, unsigned int sps, float bandwidth) {
    if (samples.empty()) {
        throw std::runtime_error("fsk_demodulate: samples is empty");
    }
    if (samples.size() % sps != 0) {
        throw std::runtime_error("fsk_demodulate: samples length must be a multiple of sps");
    }
    fskdem dem = fskdem_create(bps, sps, bandwidth);

    const size_t num_symbols = samples.size() / sps;
    std::vector<unsigned int> symbols(num_symbols);
    for (size_t i = 0; i < num_symbols; ++i) {
        symbols[i] = fskdem_demodulate(
            dem, as_liquid(const_cast<std::complex<float>*>(&samples[i * sps])));
    }

    fskdem_destroy(dem);
    return symbols_to_bits(symbols, bps);
}

}  // namespace sigcore
