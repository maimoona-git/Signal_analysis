#include "fec_registry.hpp"

#include <algorithm>
#include <cctype>

#include "fec.hpp"
#include "fec_extra.hpp"
#include "viterbi.hpp"

namespace sigapp {

namespace {

std::vector<uint8_t> bits_to_bytes(const Bits& bits) {
    std::vector<uint8_t> bytes((bits.size() + 7) / 8, 0);
    for (size_t i = 0; i < bits.size(); ++i)
        if (bits[i]) bytes[i / 8] |= static_cast<uint8_t>(0x80u >> (i % 8));
    return bytes;
}

Bits bytes_to_bits(const std::vector<uint8_t>& bytes) {
    Bits bits(bytes.size() * 8);
    for (size_t i = 0; i < bytes.size(); ++i)
        for (int b = 0; b < 8; ++b) bits[i * 8 + b] = (bytes[i] >> (7 - b)) & 1u;
    return bits;
}

std::optional<size_t> viterbi_len(size_t n) {
    if (n < 14 || n % 2) return std::nullopt;
    const long long m = static_cast<long long>(n / 2) - 6;
    return m > 0 ? std::optional<size_t>(static_cast<size_t>(m)) : std::nullopt;
}

std::optional<size_t> block_len(size_t n, size_t block_bits, size_t k_bytes) {
    if (n < block_bits || n % block_bits) return std::nullopt;
    return (n / block_bits) * k_bytes;
}

std::optional<size_t> punctured_len(size_t n, unsigned num, unsigned den) {
    const long long guess = static_cast<long long>(n) * num / den - 6;
    for (long long m = std::max<long long>(1, guess - 8); m <= guess + 8; ++m) {
        if (sigcore::viterbi_punctured_length(static_cast<size_t>(m), num, den) == n) return static_cast<size_t>(m);
    }
    return std::nullopt;
}

std::vector<FecKind> build() {
    std::vector<FecKind> k;
    {
        FecKind v;
        v.name = "viterbi"; v.soft_decode = true; v.noise_baseline = 0.88; v.in_detect_params = true;
        v.encode = [](const Bits& b) { return sigcore::viterbi_encode(b); };
        v.decode = [](const Bits& b, size_t m) { return sigcore::viterbi_decode(b, m); };
        v.msg_len_for = viterbi_len;
        k.push_back(v);
    }
    {
        FecKind v;
        v.name = "ldpc"; v.soft_decode = true; v.noise_baseline = 0.75; v.in_detect_params = true;
        v.encode = [](const Bits& b) { return sigcore::ldpc_encode(b); };
        v.decode = [](const Bits& b, size_t m) { return sigcore::ldpc_decode(b, m); };
        v.msg_len_for = [](size_t n) -> std::optional<size_t> {
            if (n < 16 || n % 16) return std::nullopt;
            return (n / 16) * 8;
        };
        k.push_back(v);
    }
    {
        FecKind v;
        v.name = "rs"; v.msg_len_in_bytes = true; v.noise_baseline = 0.98; v.in_detect_params = true;
        v.encode = [](const Bits& b) { return bytes_to_bits(sigcore::rs_encode(bits_to_bytes(b))); };
        v.decode = [](const Bits& b, size_t m) { return bytes_to_bits(sigcore::rs_decode(bits_to_bytes(b), m)); };
        v.msg_len_for = [](size_t n) { return block_len(n, 15 * 8, 13); };
        k.push_back(v);
    }
    {
        FecKind v;
        v.name = "concatenated"; v.msg_len_in_bytes = true; v.noise_baseline = 0.95; v.in_detect_params = true;
        v.encode = [](const Bits& b) { return sigcore::concatenated_encode(bits_to_bytes(b)); };
        v.decode = [](const Bits& b, size_t m) { return bytes_to_bits(sigcore::concatenated_decode(b, m)); };
        v.msg_len_for = [](size_t n) -> std::optional<size_t> {
            if (n < 14 || n % 2) return std::nullopt;
            return block_len(n / 2 - 6, 15 * 8, 13);
        };
        k.push_back(v);
    }
    // Noise agreement measured on random input at 200 message bits: rate 2/3 mean 0.912
    // max 0.932; rate 3/4 mean 0.933 max 0.949 -- which is why these two demand 0.98.
    for (auto [name, num, den, baseline] : {std::tuple<const char*, unsigned, unsigned, double>{"viterbi23", 2, 3, 0.94},
                                            {"viterbi34", 3, 4, 0.955}}) {
        FecKind v;
        v.name = name; v.noise_baseline = baseline; v.verify_threshold = 0.98;
        if (num == 2) v.min_alignment_margin = 0.005;
        const unsigned a = num, b = den;
        v.encode = [a, b](const Bits& bits) { return sigcore::viterbi_punctured_encode(bits, a, b); };
        v.decode = [a, b](const Bits& bits, size_t m) { return sigcore::viterbi_punctured_decode(bits, m, a, b); };
        v.msg_len_for = [a, b](size_t n) { return punctured_len(n, a, b); };
        k.push_back(v);
    }
    {
        FecKind v;
        v.name = "rs255"; v.msg_len_in_bytes = true; v.noise_baseline = 0.98; v.verify_threshold = 0.98;
        v.encode = [](const Bits& b) { return bytes_to_bits(sigcore::rs255_encode_bytes(bits_to_bytes(b))); };
        v.decode = [](const Bits& b, size_t m) { return bytes_to_bits(sigcore::rs255_decode_bytes(bits_to_bytes(b), m)); };
        v.msg_len_for = [](size_t n) { return block_len(n, 255 * 8, 223); };
        k.push_back(v);
    }
    return k;
}

}  // namespace

const std::vector<FecKind>& fec_kinds() {
    static const std::vector<FecKind> kinds = build();
    return kinds;
}

const FecKind* find_fec(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const auto& k : fec_kinds())
        if (k.name == lower) return &k;
    return nullptr;
}

std::vector<std::string> fec_detect_kinds() {
    std::vector<std::string> names;
    for (const auto& k : fec_kinds())
        if (k.in_detect_params) names.push_back(k.name);
    return names;
}

}  // namespace sigapp
