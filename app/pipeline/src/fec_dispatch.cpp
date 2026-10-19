#include "fec_dispatch.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include "fec.hpp"
#include "fec_registry.hpp"
#include "viterbi.hpp"

namespace sigapp {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

[[noreturn]] void unknown(const std::string& scheme) {
    std::string names;
    std::vector<std::string> sorted;
    for (const auto& k : fec_kinds()) sorted.push_back(k.name);
    std::sort(sorted.begin(), sorted.end());
    for (const auto& n : sorted) names += (names.empty() ? "'" : ", '") + n + "'";
    throw std::invalid_argument("unknown FEC scheme: '" + scheme + "' (expected one of [" + names + "])");
}

}  // namespace

Bits fec_encode(const Bits& bits, const std::string& scheme) {
    const FecKind* k = find_fec(scheme);
    if (!k) unknown(scheme);
    return k->encode(bits);
}

Bits fec_decode(const Bits& bits, const std::string& scheme, size_t msg_len) {
    const FecKind* k = find_fec(scheme);
    if (!k) unknown(scheme);
    return k->decode(bits, msg_len);
}

bool fec_supports_soft_decode(const std::string& scheme) {
    const FecKind* k = find_fec(scheme);
    return k && k->soft_decode;
}

Bits fec_decode_soft(const sigcore::LLRBuffer& llrs, const std::string& scheme, size_t msg_len) {
    const std::string s = lower(scheme);
    if (s == "viterbi") return sigcore::viterbi_decode_soft(llrs, msg_len);
    if (s == "ldpc") return sigcore::ldpc_decode_soft(llrs, msg_len);
    if (find_fec(scheme)) {
        throw std::invalid_argument("fec_decode_soft: '" + scheme + "' has no soft decoder (RS decodes on hard symbols)");
    }
    unknown(scheme);
}

}  // namespace sigapp
