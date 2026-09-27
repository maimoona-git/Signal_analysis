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

namespace {

template <class T>
std::vector<T> interleave_t(const std::vector<T>& data, const InterleaverSpec& spec) {
    const std::string kind = lower(spec.kind);
    if (kind == "block") return sigcore::block_interleave(data, need(spec.rows, "block", "rows"), need(spec.cols, "block", "cols"));
    if (kind == "diagonal") return sigcore::diagonal_interleave(data, need(spec.rows, "diagonal", "rows"), need(spec.cols, "diagonal", "cols"));
    if (kind == "pseudo_random") return sigcore::pseudo_random_interleave(data, need(spec.seed, "pseudo_random", "seed"));
    if (kind == "convolutional") {
        return sigcore::convolutional_interleave(data, need(spec.branches, "convolutional", "branches"),
                                                 need(spec.delay_increment, "convolutional", "delay_increment"));
    }
    unknown(spec.kind);
}

template <class T>
std::vector<T> deinterleave_t(const std::vector<T>& data, const InterleaverSpec& spec) {
    const std::string kind = lower(spec.kind);
    if (kind == "block") return sigcore::block_deinterleave(data, need(spec.rows, "block", "rows"), need(spec.cols, "block", "cols"));
    if (kind == "diagonal") return sigcore::diagonal_deinterleave(data, need(spec.rows, "diagonal", "rows"), need(spec.cols, "diagonal", "cols"));
    if (kind == "pseudo_random") return sigcore::pseudo_random_deinterleave(data, need(spec.seed, "pseudo_random", "seed"));
    if (kind == "convolutional") {
        return sigcore::convolutional_deinterleave(data, need(spec.branches, "convolutional", "branches"),
                                                   need(spec.delay_increment, "convolutional", "delay_increment"));
    }
    unknown(spec.kind);
}

}  // namespace

Bits interleave(const Bits& bits, const InterleaverSpec& spec) { return interleave_t(bits, spec); }
Bits deinterleave(const Bits& bits, const InterleaverSpec& spec) { return deinterleave_t(bits, spec); }
sigcore::LLRBuffer interleave(const sigcore::LLRBuffer& llrs, const InterleaverSpec& spec) { return interleave_t(llrs, spec); }
sigcore::LLRBuffer deinterleave(const sigcore::LLRBuffer& llrs, const InterleaverSpec& spec) { return deinterleave_t(llrs, spec); }

}  // namespace sigapp
