#include "detect.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <vector>

#include "correlate.hpp"
#include "fec_dispatch.hpp"
#include "fec_registry.hpp"

namespace sigapp {

namespace {

constexpr double kMaxFecRoundTripBer = 0.05;
constexpr size_t kMaxRunLength = 12;

// Tie-break bonus (smaller than one header repeat) between candidates that
// score equally: see the note in app/detect.py.
constexpr std::array<double, 4> kFecBonus = {0.1, 0.2, 0.3, 0.4};



// Each scheme has one fixed rate, so the message length follows from the
// encoded length (nullopt = this length cannot be a codeword of the scheme).
std::optional<size_t> fec_msg_len(const std::string& fec, size_t encoded_len) {
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
    throw std::invalid_argument("unknown FEC scheme: '" + fec + "'");
}

size_t max_run_length(const Bits& bits) {
    if (bits.empty()) return 0;
    size_t best = 1, run = 1;
    for (size_t i = 1; i < bits.size(); ++i) {
        run = (bits[i] == bits[i - 1]) ? run + 1 : 1;
        best = std::max(best, run);
    }
    return best;
}

// Recurrences of the stream's own first `header_len` bits (the self-match at 0
// counts). `check_run` rejects degenerate, highly repetitive candidates; it is
// only applied where no FEC round-trip already guards the candidate.
int header_repeat_score(const Bits& bits, size_t header_len, bool check_run) {
    if (bits.size() < header_len * 3) return 0;
    if (check_run && max_run_length(bits) > kMaxRunLength) return 0;
    const Bits header(bits.begin(), bits.begin() + static_cast<long>(header_len));
    return static_cast<int>(find_pattern_occurrences(bits, header, 0).size());
}

// Re-encodes `decoded` and compares with `received`: a correct FEC hypothesis
// round-trips near-exactly, a wrong one generally does not.
double fec_round_trip_ber(const std::string& fec, const Bits& decoded, const Bits& received) {
    const Bits re_encoded = fec_encode(decoded, fec);
    const size_t n = std::min(re_encoded.size(), received.size());
    if (n == 0) return 1.0;
    size_t diff = 0;
    for (size_t i = 0; i < n; ++i) diff += (re_encoded[i] != received[i]);
    return static_cast<double>(diff) / static_cast<double>(n);
}

struct Best {
    int score = 0;
    std::optional<InterleaverSpec> kind;
    std::optional<std::string> fec;
    std::optional<size_t> msg_len;
    Bits deinterleaved;
    Bits working;
};

}  // namespace

DetectionResult detect_params(const Bits& bits, std::optional<size_t> header_len, int score_floor,
                              unsigned int pseudo_random_seed_range) {
    if (bits.empty()) {
        throw std::invalid_argument("detect_params: bits is empty");
    }

    const size_t hlen = (header_len.has_value() && *header_len != 0)
                            ? *header_len
                            : std::min<size_t>(16, std::max<size_t>(4, bits.size() / 8));

    std::optional<Best> best;
    double best_rank = -1.0;

    for (const auto& candidate : interleaver_candidates(bits.size(), pseudo_random_seed_range)) {
        Bits deinterleaved;
        try {
            deinterleaved = candidate.has_value() ? deinterleave(bits, *candidate) : bits;
        } catch (const std::exception&) {
            continue;
        }

        if (candidate.has_value() && candidate->kind == "convolutional") {
            // The deinterleaver's leading `offset` bits are FIFO flush content, not data.
            const size_t b = *candidate->branches;
            const size_t offset = b * (b - 1) * *candidate->delay_increment;
            deinterleaved.erase(deinterleaved.begin(),
                                deinterleaved.begin() + static_cast<long>(std::min(offset, deinterleaved.size())));
            if (deinterleaved.empty()) continue;
        }

        struct Option {
            std::optional<std::string> fec;
            std::optional<size_t> msg_len;
            Bits working;
            double bonus;
        };
        std::vector<Option> options;
        options.push_back({std::nullopt, std::nullopt, deinterleaved, 0.0});
        const std::vector<std::string> kFecKinds = fec_detect_kinds();  // the original's four, in its order
        for (size_t k = 0; k < kFecKinds.size(); ++k) {
            const std::string fec = kFecKinds[k];
            const auto msg_len = fec_msg_len(fec, deinterleaved.size());
            if (!msg_len.has_value()) continue;
            try {
                Bits decoded = fec_decode(deinterleaved, fec, *msg_len);
                const double ber = fec_round_trip_ber(fec, decoded, deinterleaved);
                if (ber > kMaxFecRoundTripBer) continue;
                options.push_back({fec, msg_len, std::move(decoded), kFecBonus[k]});
            } catch (const std::exception&) {
                continue;
            }
        }

        for (auto& option : options) {
            const int score = header_repeat_score(option.working, hlen, !option.fec.has_value());
            const double rank = score + option.bonus;
            if (rank > best_rank) {
                best_rank = rank;
                best = Best{score, candidate, option.fec, option.msg_len, deinterleaved, std::move(option.working)};
            }
        }
    }

    // The identity candidate always yields a rank >= 0, so a best always exists.
    if (best->score < score_floor) {
        DetectionResult none;
        none.score = best->score;
        none.working_bits = bits;
        return none;
    }

    DetectionResult result;
    result.interleaver = best->kind;
    result.fec = best->fec;
    result.fec_msg_len = best->msg_len;
    result.score = best->score;
    if (best->kind.has_value()) result.deinterleaved_bits = best->deinterleaved;
    if (best->fec.has_value()) result.decoded_bits = best->working;
    result.working_bits = std::move(best->working);
    return result;
}

}  // namespace sigapp
