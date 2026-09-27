#include "fec_dispatch.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include "fec.hpp"
#include "viterbi.hpp"

namespace sigapp {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// np.packbits (big-endian bit order) after zero-padding to a whole byte.
std::vector<uint8_t> bits_to_bytes(const Bits& bits) {
    std::vector<uint8_t> bytes((bits.size() + 7) / 8, 0);
    for (size_t i = 0; i < bits.size(); ++i) {
        if (bits[i]) bytes[i / 8] |= static_cast<uint8_t>(0x80u >> (i % 8));
    }
    return bytes;
}

// np.unpackbits
Bits bytes_to_bits(const std::vector<uint8_t>& bytes) {
    Bits bits(bytes.size() * 8);
    for (size_t i = 0; i < bytes.size(); ++i) {
        for (int b = 0; b < 8; ++b) bits[i * 8 + b] = (bytes[i] >> (7 - b)) & 1u;
    }
    return bits;
}

[[noreturn]] void unknown(const std::string& scheme) {
    throw std::invalid_argument("unknown FEC scheme: '" + scheme +
                                "' (expected one of ['concatenated', 'ldpc', 'rs', 'viterbi'])");
}

}  // namespace

Bits fec_encode(const Bits& bits, const std::string& scheme) {
    const std::string s = lower(scheme);
    if (s == "viterbi") return sigcore::viterbi_encode(bits);
    if (s == "ldpc") return sigcore::ldpc_encode(bits);
    if (s == "rs") return bytes_to_bits(sigcore::rs_encode(bits_to_bytes(bits)));
    if (s == "concatenated") return sigcore::concatenated_encode(bits_to_bytes(bits));
    unknown(scheme);
}

Bits fec_decode(const Bits& bits, const std::string& scheme, size_t msg_len) {
    const std::string s = lower(scheme);
    if (s == "viterbi") return sigcore::viterbi_decode(bits, msg_len);
    if (s == "ldpc") return sigcore::ldpc_decode(bits, msg_len);
    if (s == "rs") return bytes_to_bits(sigcore::rs_decode(bits_to_bytes(bits), msg_len));
    if (s == "concatenated") return bytes_to_bits(sigcore::concatenated_decode(bits, msg_len));
    unknown(scheme);
}

bool fec_supports_soft_decode(const std::string& scheme) {
    const std::string s = lower(scheme);
    return s == "viterbi" || s == "ldpc";
}

Bits fec_decode_soft(const sigcore::LLRBuffer& llrs, const std::string& scheme, size_t msg_len) {
    const std::string s = lower(scheme);
    if (s == "viterbi") return sigcore::viterbi_decode_soft(llrs, msg_len);
    if (s == "ldpc") return sigcore::ldpc_decode_soft(llrs, msg_len);
    if (s == "rs" || s == "concatenated") {
        throw std::invalid_argument("fec_decode_soft: '" + scheme + "' has no soft decoder (RS decodes on hard symbols)");
    }
    unknown(scheme);
}

}  // namespace sigapp
