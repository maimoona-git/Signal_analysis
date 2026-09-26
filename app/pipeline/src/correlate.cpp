#include "correlate.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>

namespace sigapp {

namespace {

// Bit i lives at bit (i % 64) of word i / 64; one zero word of padding at the
// end so a shifted read never runs off the array.
std::vector<uint64_t> pack(const Bits& bits) {
    std::vector<uint64_t> words(bits.size() / 64 + 2, 0);
    for (size_t i = 0; i < bits.size(); ++i) {
        if (bits[i]) words[i / 64] |= uint64_t{1} << (i % 64);
    }
    return words;
}

int popcount64(uint64_t x) { return __builtin_popcountll(x); }

// sum_i s[i] * s[i+lag] for s = 2*bit-1, i.e. (n - lag) - 2 * (mismatching pairs).
long long correlation_at_lag(const std::vector<uint64_t>& w, size_t n, size_t lag) {
    const size_t len = n - lag;  // compared pairs
    const size_t q = lag / 64, r = lag % 64;
    size_t mismatches = 0;
    const size_t full = len / 64, tail = len % 64;
    auto shifted = [&](size_t j) {
        uint64_t lo = w[j + q] >> r;
        return r == 0 ? lo : (lo | (w[j + q + 1] << (64 - r)));
    };
    for (size_t j = 0; j < full; ++j) mismatches += popcount64(w[j] ^ shifted(j));
    if (tail) {
        const uint64_t mask = (uint64_t{1} << tail) - 1;
        mismatches += popcount64((w[full] ^ shifted(full)) & mask);
    }
    return static_cast<long long>(len) - 2 * static_cast<long long>(mismatches);
}

}  // namespace

std::vector<double> autocorrelation(const Bits& bits) {
    if (bits.size() < 2) {
        throw std::invalid_argument("autocorrelation: need at least 2 bits");
    }
    const size_t n = bits.size();
    const auto words = pack(bits);
    std::vector<double> out(n - 1);
    for (size_t lag = 1; lag < n; ++lag) {
        out[lag - 1] = static_cast<double>(correlation_at_lag(words, n, lag)) / static_cast<double>(n);
    }
    return out;
}

std::pair<int, double> detect_frame_period(const Bits& bits, int min_period, std::optional<int> max_period) {
    if (bits.size() < 2) {
        throw std::invalid_argument("autocorrelation: need at least 2 bits");
    }
    const int n = static_cast<int>(bits.size());
    int max_p = (max_period.has_value() && *max_period != 0) ? *max_period : n / 2;
    if (max_p <= min_period) {
        throw std::invalid_argument("detect_frame_period: signal too short for the requested period range");
    }
    if (min_period < 1) {
        throw std::invalid_argument("detect_frame_period: min_period must be >= 1");
    }
    const int hi = std::min(max_p, n - 1);  // lags beyond n-1 do not exist (numpy slicing clamps too)
    if (min_period > hi) {
        throw std::invalid_argument("detect_frame_period: empty period range");
    }

    const auto words = pack(bits);
    int best_period = min_period;
    long long best = correlation_at_lag(words, bits.size(), static_cast<size_t>(min_period));
    for (int lag = min_period + 1; lag <= hi; ++lag) {
        const long long c = correlation_at_lag(words, bits.size(), static_cast<size_t>(lag));
        if (c > best) {  // strict: first maximum wins, like np.argmax
            best = c;
            best_period = lag;
        }
    }
    return {best_period, static_cast<double>(best) / static_cast<double>(n)};
}

std::vector<size_t> find_pattern_occurrences(const Bits& bits, const Bits& pattern, int max_bit_errors) {
    const size_t n = bits.size(), m = pattern.size();
    std::vector<size_t> positions;
    if (m == 0 || m > n) return positions;
    for (size_t i = 0; i + m <= n; ++i) {
        int mismatches = 0;
        for (size_t j = 0; j < m; ++j) {
            if ((bits[i + j] != 0) != (pattern[j] != 0) && ++mismatches > max_bit_errors) break;
        }
        if (mismatches <= max_bit_errors) positions.push_back(i);
    }
    return positions;
}

std::string bits_to_text(const Bits& bits) {
    if (bits.empty()) return "";
    const size_t nbytes = bits.size() / 8;  // a ragged tail is dropped
    std::string raw(nbytes, '\0');
    for (size_t i = 0; i < nbytes; ++i) {
        unsigned byte = 0;
        for (int b = 0; b < 8; ++b) byte = (byte << 1) | (bits[i * 8 + b] ? 1u : 0u);
        raw[i] = static_cast<char>(byte);
    }

    auto to_hex = [&] {
        static const char* digits = "0123456789abcdef";
        std::string hex;
        for (unsigned char c : raw) {
            hex += digits[c >> 4];
            hex += digits[c & 15];
        }
        return hex;
    };
    for (unsigned char c : raw) {
        if (c >= 128) return to_hex();                                   // not ASCII
        if (!(c == '\n' || c == '\r' || c == '\t' || (c >= 32 && c < 127))) return to_hex();  // not printable
    }
    return raw;
}

FrameBoundaries find_header_payload_boundaries(const Bits& bits, const std::optional<Bits>& header_pattern,
                                               size_t header_len, int max_bit_errors, int min_period) {
    if (bits.empty()) {
        throw std::invalid_argument("find_header_payload_boundaries: bits is empty");
    }

    FrameBoundaries out;
    out.header_pattern = header_pattern.has_value()
                             ? *header_pattern
                             : Bits(bits.begin(), bits.begin() + static_cast<long>(std::min(header_len, bits.size())));
    out.header_positions = find_pattern_occurrences(bits, out.header_pattern, max_bit_errors);
    const auto& positions = out.header_positions;

    if (positions.size() >= 2) {
        // Most common gap; on a tie the smallest gap (np.bincount(...).argmax()).
        std::map<size_t, size_t> counts;
        for (size_t i = 1; i < positions.size(); ++i) ++counts[positions[i] - positions[i - 1]];
        size_t best_gap = 0, best_count = 0;
        for (const auto& [gap, count] : counts) {
            if (count > best_count) {
                best_gap = gap;
                best_count = count;
            }
        }
        out.frame_period = static_cast<int>(best_gap);
    } else if (static_cast<long long>(bits.size()) > 2LL * min_period) {
        try {
            out.frame_period = detect_frame_period(bits, min_period).first;
        } catch (const std::invalid_argument&) {
            out.frame_period = std::nullopt;
        }
    }

    for (size_t idx = 0; idx < positions.size(); ++idx) {
        const size_t payload_start = positions[idx] + out.header_pattern.size();
        const size_t payload_end = idx + 1 < positions.size() ? positions[idx + 1] : bits.size();
        if (payload_start < payload_end) {
            out.payloads.emplace_back(bits.begin() + static_cast<long>(payload_start),
                                      bits.begin() + static_cast<long>(payload_end));
        }
    }
    return out;
}

}  // namespace sigapp
