#include "param_search.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

#include "correlate.hpp"
#include "fec_dispatch.hpp"

namespace sigapp {

namespace {

constexpr unsigned kMaxGridDim = 256;
constexpr unsigned kMaxGridAspectRatio = 8;
constexpr std::array<const char*, 4> kFecKinds = {"viterbi", "ldpc", "rs", "concatenated"};

std::vector<std::pair<unsigned, unsigned>> divisor_pairs(size_t n) {
    std::vector<std::pair<unsigned, unsigned>> pairs;
    const size_t max_rows = std::min<size_t>(n, kMaxGridDim);
    for (size_t rows = 2; rows <= max_rows; ++rows) {
        if (n % rows != 0) continue;
        const size_t cols = n / rows;
        if (cols >= 2 && cols <= kMaxGridDim && std::max(rows, cols) <= kMaxGridAspectRatio * std::min(rows, cols)) {
            pairs.emplace_back(static_cast<unsigned>(rows), static_cast<unsigned>(cols));
        }
    }
    return pairs;
}

std::vector<std::optional<InterleaverSpec>> interleaver_candidates(size_t n, unsigned seed_range) {
    std::vector<std::optional<InterleaverSpec>> out;
    out.emplace_back(std::nullopt);
    for (const auto& [rows, cols] : divisor_pairs(n)) {
        out.emplace_back(InterleaverSpec::block(rows, cols));
        out.emplace_back(InterleaverSpec::diagonal(rows, cols));
    }
    for (unsigned branches = 2; branches <= 8; ++branches) {
        for (unsigned delay = 1; delay <= 4; ++delay) {
            if (static_cast<size_t>(branches) * (branches - 1) * delay < n) {
                out.emplace_back(InterleaverSpec::convolutional(branches, delay));
            }
        }
    }
    for (unsigned seed = 0; seed < seed_range; ++seed) out.emplace_back(InterleaverSpec::pseudo_random(seed));
    return out;
}

// Same fixed-rate arithmetic as detect.cpp's own (kept independent on purpose: see
// param_search.hpp's header comment on why this is a separate module).
std::optional<size_t> fec_msg_len_for(const std::string& fec, size_t encoded_len) {
    if (fec == "viterbi") {
        if (encoded_len < 14 || encoded_len % 2) return std::nullopt;
        const long long msg_len = static_cast<long long>(encoded_len / 2) - 6;
        return msg_len > 0 ? std::optional<size_t>(static_cast<size_t>(msg_len)) : std::nullopt;
    }
    if (fec == "ldpc") {
        if (encoded_len < 16 || encoded_len % 16) return std::nullopt;
        return (encoded_len / 16) * 8;
    }
    if (fec == "rs") {
        constexpr size_t block_bits = 15 * 8;
        if (encoded_len < block_bits || encoded_len % block_bits) return std::nullopt;
        return (encoded_len / block_bits) * 13;
    }
    if (fec == "concatenated") {
        if (encoded_len < 14 || encoded_len % 2) return std::nullopt;
        const size_t rs_bits_len = encoded_len / 2 - 6;
        constexpr size_t block_bits = 15 * 8;
        if (rs_bits_len < block_bits || rs_bits_len % block_bits) return std::nullopt;
        return (rs_bits_len / block_bits) * 13;
    }
    return std::nullopt;
}

int header_repeat_score(const Bits& bits, size_t header_len) {
    if (bits.size() < header_len * 3 || header_len == 0) return 0;
    const Bits header(bits.begin(), bits.begin() + static_cast<long>(header_len));
    return static_cast<int>(find_pattern_occurrences(bits, header, 0).size());
}

// A FEC decoder does not need a real codeword to "succeed": Viterbi and LDPC both
// always produce SOME output for any input (a nearest-codeword / best-effort search),
// and even Reed-Solomon occasionally gets lucky. Measured on pure noise (nonlinear,
// not testutil's linear xorshift -- see test_param_search.cpp's own note on that),
// decoding-then-re-encoding random bits still agrees with the original noise far more
// than chance: ~87% for Viterbi (a rate-1/2 code's covering radius is well under half
// its length), ~59-72% for LDPC, and RS/concatenated mostly THROW on pure noise
// (self-filtering) but occasionally reach ~97%. A flat "agreement > 0.5" floor would
// therefore false-positive on nearly every Viterbi/LDPC candidate tried against noise
// -- found the same way as Feature 2's random-matrix baseline, by measuring, not
// assuming. Confidence instead reports how far agreement clears each scheme's own
// measured noise baseline, normalised into [0, 1]; a real decode still reads close to
// 1.0 (it agrees almost entirely), while noise reads close to 0.
double noise_baseline(const std::string& fec) {
    if (fec == "viterbi") return 0.88;
    if (fec == "ldpc") return 0.75;
    if (fec == "rs") return 0.98;
    return 0.95;  // concatenated: almost never even completes on noise; a conservative default
}

// See param_search.hpp: a real decode match (`reencode_agreement`) is the primary
// signal; a header-repeat count is used only when there is nothing else to check
// against, and is capped well below what any genuine decode match reports, so it can
// never outrank one.
double confidence_of(const std::optional<double>& agreement, const std::optional<std::string>& fec,
                     int header_score) {
    if (agreement.has_value()) {
        const double baseline = noise_baseline(*fec);
        return std::clamp((*agreement - baseline) / (1.0 - baseline), 0.0, 1.0);
    }
    constexpr double kNoFecCap = 0.45;
    constexpr double kExpectedRepeatsForFullCap = 8.0;  // a strongly repeating header saturates the cap
    return std::min(kNoFecCap, kNoFecCap * static_cast<double>(header_score) / kExpectedRepeatsForFullCap);
}

}  // namespace

ParamSearchResult search_params(const Bits& bits_in, const ParamSearchOptions& options) {
    if (bits_in.empty()) {
        throw std::invalid_argument("search_params: bits is empty");
    }

    Bits truncated_storage;
    if (options.max_analysis_bits > 0 && bits_in.size() > options.max_analysis_bits) {
        truncated_storage.assign(bits_in.begin(), bits_in.begin() + static_cast<long>(options.max_analysis_bits));
    }
    const Bits& full_bits = truncated_storage.empty() ? bits_in : truncated_storage;

    ParamSearchResult result;

    for (unsigned offset = 0; offset <= options.max_bit_offset; ++offset) {
        if (offset >= full_bits.size()) break;
        const Bits bits(full_bits.begin() + static_cast<long>(offset), full_bits.end());
        const size_t hlen = std::min<size_t>(16, std::max<size_t>(4, bits.size() / 8));

        for (const auto& interleaver : interleaver_candidates(bits.size(), options.pseudo_random_seed_range)) {
            Bits deinterleaved;
            try {
                deinterleaved = interleaver.has_value() ? deinterleave(bits, *interleaver) : bits;
            } catch (const std::exception&) {
                continue;
            }
            if (interleaver.has_value() && interleaver->kind == "convolutional") {
                const size_t b = *interleaver->branches;
                const size_t flush = b * (b - 1) * *interleaver->delay_increment;
                deinterleaved.erase(deinterleaved.begin(),
                                    deinterleaved.begin() + static_cast<long>(std::min(flush, deinterleaved.size())));
                if (deinterleaved.empty()) continue;
            }

            // The no-FEC candidate itself.
            {
                ParamCandidate c;
                c.bit_offset = offset;
                c.interleaver = interleaver;
                c.header_repeat_score = header_repeat_score(deinterleaved, hlen);
                c.confidence = confidence_of(std::nullopt, std::nullopt, c.header_repeat_score);
                c.decoded_bits = deinterleaved.size();
                result.candidates.push_back(std::move(c));
            }

            for (const char* fec : kFecKinds) {
                const auto msg_len = fec_msg_len_for(fec, deinterleaved.size());
                if (!msg_len.has_value()) continue;
                try {
                    const Bits decoded = fec_decode(deinterleaved, fec, *msg_len);
                    const Bits re_encoded = fec_encode(decoded, fec);
                    const size_t n = std::min(re_encoded.size(), deinterleaved.size());
                    size_t matches = 0;
                    for (size_t i = 0; i < n; ++i) matches += (re_encoded[i] == deinterleaved[i]);
                    const double agreement = n > 0 ? static_cast<double>(matches) / n : 0.0;

                    ParamCandidate c;
                    c.bit_offset = offset;
                    c.interleaver = interleaver;
                    c.fec = fec;
                    c.fec_msg_len = msg_len;
                    c.reencode_agreement = agreement;
                    c.header_repeat_score = header_repeat_score(decoded, hlen);
                    c.confidence = confidence_of(agreement, std::string(fec), c.header_repeat_score);
                    c.decoded_bits = decoded.size();
                    result.candidates.push_back(std::move(c));
                } catch (const std::exception&) {
                    continue;
                }
            }
        }
    }

    std::stable_sort(result.candidates.begin(), result.candidates.end(),
                     [](const ParamCandidate& a, const ParamCandidate& b) { return a.confidence > b.confidence; });
    if (result.candidates.size() > options.max_candidates_kept) {
        result.candidates.resize(options.max_candidates_kept);
    }

    if (!result.candidates.empty() && result.candidates.front().confidence >= options.confidence_floor) {
        result.detected = true;
        result.best_index = 0;
    }
    return result;
}

Bits decode_candidate(const Bits& bits, const ParamCandidate& candidate) {
    if (candidate.bit_offset >= bits.size()) {
        throw std::invalid_argument("decode_candidate: bit_offset is past the end of bits");
    }
    Bits working(bits.begin() + static_cast<long>(candidate.bit_offset), bits.end());

    if (candidate.interleaver.has_value()) {
        working = deinterleave(working, *candidate.interleaver);
        if (candidate.interleaver->kind == "convolutional") {
            const size_t b = *candidate.interleaver->branches;
            const size_t flush = b * (b - 1) * *candidate.interleaver->delay_increment;
            working.erase(working.begin(), working.begin() + static_cast<long>(std::min(flush, working.size())));
        }
    }
    if (candidate.fec.has_value()) {
        working = fec_decode(working, *candidate.fec, *candidate.fec_msg_len);
    }
    return working;
}

}  // namespace sigapp
