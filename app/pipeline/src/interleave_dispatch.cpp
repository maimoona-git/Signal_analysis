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

namespace {

constexpr unsigned int kMaxGridDim = 256;
constexpr unsigned int kMaxGridAspectRatio = 8;

std::vector<std::pair<unsigned int, unsigned int>> divisor_pairs(size_t n) {
    std::vector<std::pair<unsigned int, unsigned int>> pairs;
    const size_t max_rows = std::min<size_t>(n, kMaxGridDim);
    for (size_t rows = 2; rows <= max_rows; ++rows) {
        if (n % rows != 0) continue;
        const size_t cols = n / rows;
        if (cols >= 2 && cols <= kMaxGridDim && std::max(rows, cols) <= kMaxGridAspectRatio * std::min(rows, cols)) {
            pairs.emplace_back(static_cast<unsigned int>(rows), static_cast<unsigned int>(cols));
        }
    }
    return pairs;
}

}  // namespace

std::vector<std::optional<InterleaverSpec>> interleaver_candidates(size_t n, unsigned int seed_range) {
    std::vector<std::optional<InterleaverSpec>> candidates;
    candidates.emplace_back(std::nullopt);
    for (const auto& [rows, cols] : divisor_pairs(n)) {
        candidates.emplace_back(InterleaverSpec::block(rows, cols));
        candidates.emplace_back(InterleaverSpec::diagonal(rows, cols));
    }
    for (unsigned int branches = 2; branches <= 8; ++branches) {
        for (unsigned int delay = 1; delay <= 4; ++delay) {
            if (static_cast<size_t>(branches) * (branches - 1) * delay < n) {
                candidates.emplace_back(InterleaverSpec::convolutional(branches, delay));
            }
        }
    }
    for (unsigned int seed = 0; seed < seed_range; ++seed) candidates.emplace_back(InterleaverSpec::pseudo_random(seed));
    return candidates;
}

}  // namespace sigapp
