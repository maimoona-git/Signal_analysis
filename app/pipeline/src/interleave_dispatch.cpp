#include "interleave_dispatch.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include "interleave.hpp"

namespace sigapp {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

uint32_t need(const std::optional<uint32_t>& v, const char* kind, const char* name) {
    if (!v.has_value()) {
        throw std::invalid_argument(std::string("interleaver '") + kind + "' requires parameter '" + name + "'");
    }
    return *v;
}

[[noreturn]] void unknown(const std::string& kind) {
    throw std::invalid_argument("unknown interleaver: '" + kind +
                                "' (expected one of ['block', 'convolutional', 'diagonal', 'pseudo_random'])");
}

}  // namespace

Bits interleave(const Bits& bits, const InterleaverSpec& spec) {
    const std::string kind = lower(spec.kind);
    if (kind == "block") return sigcore::block_interleave(bits, need(spec.rows, "block", "rows"), need(spec.cols, "block", "cols"));
    if (kind == "diagonal") return sigcore::diagonal_interleave(bits, need(spec.rows, "diagonal", "rows"), need(spec.cols, "diagonal", "cols"));
    if (kind == "pseudo_random") return sigcore::pseudo_random_interleave(bits, need(spec.seed, "pseudo_random", "seed"));
    if (kind == "convolutional") {
        return sigcore::convolutional_interleave(bits, need(spec.branches, "convolutional", "branches"),
                                                 need(spec.delay_increment, "convolutional", "delay_increment"));
    }
    unknown(spec.kind);
}

Bits deinterleave(const Bits& bits, const InterleaverSpec& spec) {
    const std::string kind = lower(spec.kind);
    if (kind == "block") return sigcore::block_deinterleave(bits, need(spec.rows, "block", "rows"), need(spec.cols, "block", "cols"));
    if (kind == "diagonal") return sigcore::diagonal_deinterleave(bits, need(spec.rows, "diagonal", "rows"), need(spec.cols, "diagonal", "cols"));
    if (kind == "pseudo_random") return sigcore::pseudo_random_deinterleave(bits, need(spec.seed, "pseudo_random", "seed"));
    if (kind == "convolutional") {
        return sigcore::convolutional_deinterleave(bits, need(spec.branches, "convolutional", "branches"),
                                                   need(spec.delay_increment, "convolutional", "delay_increment"));
    }
    unknown(spec.kind);
}

}  // namespace sigapp
