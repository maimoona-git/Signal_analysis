#include "viterbi.hpp"

#include <climits>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace sigcore {

namespace {

constexpr unsigned int kK = 7;               // constraint length
constexpr unsigned int kNumStates = 1u << (kK - 1);  // 64
constexpr unsigned int kG1 = 0171;            // octal 171
constexpr unsigned int kG2 = 0133;            // octal 133

unsigned int parity(unsigned int x) {
    unsigned int p = 0;
    while (x) {
        p ^= (x & 1);
        x >>= 1;
    }
    return p;
}

void output_bits(unsigned int state, unsigned int input, unsigned int& out1, unsigned int& out2) {
    unsigned int window = (input << (kK - 1)) | state;
    out1 = parity(window & kG1);
    out2 = parity(window & kG2);
}

}  // namespace

std::vector<uint8_t> viterbi_encode_bits(const std::vector<uint8_t>& msg_bits) {
    if (msg_bits.empty()) {
        throw std::runtime_error("viterbi_encode_bits: msg_bits is empty");
    }

    std::vector<uint8_t> padded = msg_bits;
    padded.insert(padded.end(), kK - 1, 0);  // flush tail

    std::vector<uint8_t> out;
    out.reserve(padded.size() * 2);

    unsigned int state = 0;
    for (uint8_t bit : padded) {
        unsigned int o1, o2;
        output_bits(state, bit, o1, o2);
        out.push_back(static_cast<uint8_t>(o1));
        out.push_back(static_cast<uint8_t>(o2));
        state = ((state << 1) | bit) & (kNumStates - 1);
    }
    return out;
}

std::vector<uint8_t> viterbi_decode_bits(const std::vector<uint8_t>& coded_bits,
                                          size_t msg_len_bits) {
    if (coded_bits.empty()) {
        throw std::runtime_error("viterbi_decode_bits: coded_bits is empty");
    }
    if (coded_bits.size() % 2 != 0) {
        throw std::runtime_error("viterbi_decode_bits: coded_bits.size() must be even");
    }
    const size_t num_steps = coded_bits.size() / 2;
    const unsigned int kInf = UINT_MAX / 2;

    std::vector<unsigned int> path_metric(kNumStates, kInf);
    path_metric[0] = 0;

    // backptr[t][ns] = (prev_state, input_bit)
    std::vector<std::vector<std::pair<uint8_t, uint8_t>>> backptr(
        num_steps, std::vector<std::pair<uint8_t, uint8_t>>(kNumStates, {0, 0}));

    for (size_t t = 0; t < num_steps; ++t) {
        unsigned int r1 = coded_bits[2 * t];
        unsigned int r2 = coded_bits[2 * t + 1];

        std::vector<unsigned int> new_metric(kNumStates, kInf);
        for (unsigned int s = 0; s < kNumStates; ++s) {
            if (path_metric[s] >= kInf) continue;
            for (unsigned int u = 0; u < 2; ++u) {
                unsigned int o1, o2;
                output_bits(s, u, o1, o2);
                unsigned int bm = (o1 != r1) + (o2 != r2);
                unsigned int ns = ((s << 1) | u) & (kNumStates - 1);
                unsigned int cost = path_metric[s] + bm;
                if (cost < new_metric[ns]) {
                    new_metric[ns] = cost;
                    backptr[t][ns] = {static_cast<uint8_t>(s), static_cast<uint8_t>(u)};
                }
            }
        }
        path_metric = new_metric;
    }

    // Encoder was flushed to the zero state via the tail bits.
    std::vector<uint8_t> bits_reversed;
    bits_reversed.reserve(num_steps);
    unsigned int state = 0;
    for (size_t t = num_steps; t-- > 0;) {
        auto [prev_state, input_bit] = backptr[t][state];
        bits_reversed.push_back(input_bit);
        state = prev_state;
    }

    std::vector<uint8_t> bits(bits_reversed.rbegin(), bits_reversed.rend());
    if (bits.size() < kK - 1 || msg_len_bits > bits.size() - (kK - 1)) {
        throw std::runtime_error("viterbi_decode_bits: msg_len_bits exceeds decoded length");
    }
    bits.resize(bits.size() - (kK - 1));  // drop tail
    bits.resize(msg_len_bits);
    return bits;
}

}  // namespace sigcore
