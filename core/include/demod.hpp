#pragma once

#include <complex>
#include <cstdint>
#include <vector>

namespace sigcore {

// Mirrors a subset of liquid-dsp's modulation_scheme enum (linear modems:
// PSK, QAM, ASK/OOK). FSK is handled separately via fsk_modulate/demodulate
// since liquid-dsp models it with a distinct fskmod/fskdem object family.
enum class ModulationScheme {
    BPSK,
    QPSK,
    PSK8,
    PSK16,
    PSK32,
    PSK64,
    QAM4,
    QAM16,
    QAM32,
    QAM64,
    QAM128,
    QAM256,
    ASK2,
    ASK4,
    ASK8,
    ASK16,
    OOK,
};

// Packs a bitstream into bits-per-symbol-wide symbols (MSB first), zero
// padding the final symbol if needed.
std::vector<unsigned int> bits_to_symbols(const std::vector<uint8_t>& bits, unsigned int bps);

// Unpacks symbols back into individual bits (MSB first).
std::vector<uint8_t> symbols_to_bits(const std::vector<unsigned int>& symbols, unsigned int bps);

// Linear modulation (PSK/QAM/ASK/OOK), one complex sample per symbol.
std::vector<std::complex<float>> linear_modulate(const std::vector<uint8_t>& bits,
                                                  ModulationScheme scheme);

// Linear demodulation: hard-decision nearest-symbol recovery.
std::vector<uint8_t> linear_demodulate(const std::vector<std::complex<float>>& samples,
                                        ModulationScheme scheme);

// M-ary FSK modulation.
//  bps      : bits per symbol
//  sps      : samples per symbol (must be >= 2^bps)
//  bandwidth: total signal bandwidth, 0 < bandwidth < 0.5
std::vector<std::complex<float>> fsk_modulate(const std::vector<uint8_t>& bits, unsigned int bps,
                                               unsigned int sps, float bandwidth);

// M-ary FSK demodulation, assuming perfect symbol timing.
std::vector<uint8_t> fsk_demodulate(const std::vector<std::complex<float>>& samples,
                                     unsigned int bps, unsigned int sps, float bandwidth);

}  // namespace sigcore
