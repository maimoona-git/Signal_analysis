#include "fec.hpp"

#include "reed_solomon.hpp"
#include "viterbi.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace sigcore {

namespace {

std::vector<uint8_t> bytes_to_bits(const std::vector<uint8_t>& bytes) {
    std::vector<uint8_t> bits(bytes.size() * 8);
    for (size_t i = 0; i < bytes.size(); ++i) {
        for (int b = 0; b < 8; ++b) {
            bits[i * 8 + b] = (bytes[i] >> (7 - b)) & 0x1;
        }
    }
    return bits;
}

std::vector<uint8_t> bits_to_bytes(const std::vector<uint8_t>& bits) {
    if (bits.size() % 8 != 0) {
        throw std::runtime_error("bits_to_bytes: bits.size() must be a multiple of 8");
    }
    std::vector<uint8_t> bytes(bits.size() / 8);
    for (size_t i = 0; i < bytes.size(); ++i) {
        uint8_t byte = 0;
        for (int b = 0; b < 8; ++b) {
            byte = static_cast<uint8_t>((byte << 1) | bits[i * 8 + b]);
        }
        bytes[i] = byte;
    }
    return bytes;
}

}  // namespace

std::vector<uint8_t> viterbi_encode(const std::vector<uint8_t>& msg_bits) {
    return viterbi_encode_bits(msg_bits);
}

std::vector<uint8_t> viterbi_decode(const std::vector<uint8_t>& coded_bits, size_t msg_len_bits) {
    return viterbi_decode_bits(coded_bits, msg_len_bits);
}

std::vector<uint8_t> rs_encode(const std::vector<uint8_t>& msg) { return rs_encode_bytes(msg); }

std::vector<uint8_t> rs_decode(const std::vector<uint8_t>& encoded, size_t msg_len) {
    return rs_decode_bytes(encoded, msg_len);
}

std::vector<uint8_t> concatenated_encode(const std::vector<uint8_t>& msg) {
    if (msg.empty()) {
        throw std::runtime_error("concatenated_encode: msg is empty");
    }
    auto rs_bytes = rs_encode_bytes(msg);
    auto rs_bits = bytes_to_bits(rs_bytes);
    return viterbi_encode_bits(rs_bits);
}

std::vector<uint8_t> concatenated_decode(const std::vector<uint8_t>& encoded_bits,
                                          size_t msg_len) {
    if (encoded_bits.empty()) {
        throw std::runtime_error("concatenated_decode: encoded_bits is empty");
    }
    size_t num_rs_blocks = (msg_len + 12) / 13;  // kK=13 bytes/block (reed_solomon.cpp)
    size_t rs_bits_len = num_rs_blocks * 15 * 8;  // kN=15 bytes/block

    auto rs_bits = viterbi_decode_bits(encoded_bits, rs_bits_len);
    auto rs_bytes = bits_to_bytes(rs_bits);
    return rs_decode_bytes(rs_bytes, msg_len);
}

// --- Hand-rolled small regular LDPC block code (rate 1/2, k=8, n=16) ---
//
// Parity-check matrix H = [H1 | H2] (m=8 x n=16):
//   H1 (m x k): sparse, deterministic pattern (column weight ~3)
//   H2 (m x m): unit lower-bidiagonal "staircase" (invertible), enabling
//               O(n) systematic encoding via back-substitution.
// Decoding uses Gallager's bit-flipping Algorithm A.

namespace {

constexpr unsigned int kK = 8;   // message bits per block
constexpr unsigned int kM = 8;   // parity bits per block
constexpr unsigned int kN = kK + kM;  // codeword bits per block
constexpr unsigned int kColWeight = 3;
constexpr unsigned int kMaxIters = 20;

// H1[row][col], built once.
std::vector<std::vector<uint8_t>> build_h1() {
    std::vector<std::vector<uint8_t>> h1(kM, std::vector<uint8_t>(kK, 0));
    for (unsigned int j = 0; j < kK; ++j) {
        unsigned int rows[kColWeight] = {(j * 3 + 1) % kM, (j * 5 + 2) % kM, (j * 2 + 7) % kM};
        for (unsigned int r : rows) {
            h1[r][j] = 1;
        }
    }
    return h1;
}

// Full H[row][col], row = kM, col = kN (H1 followed by staircase H2).
std::vector<std::vector<uint8_t>> build_h() {
    auto h1 = build_h1();
    std::vector<std::vector<uint8_t>> h(kM, std::vector<uint8_t>(kN, 0));
    for (unsigned int r = 0; r < kM; ++r) {
        for (unsigned int c = 0; c < kK; ++c) {
            h[r][c] = h1[r][c];
        }
        h[r][kK + r] = 1;
        if (r > 0) {
            h[r][kK + r - 1] = 1;
        }
    }
    return h;
}

std::vector<uint8_t> encode_block(const std::vector<uint8_t>& msg) {
    static const auto h1 = build_h1();
    std::vector<uint8_t> codeword(kN, 0);
    for (unsigned int j = 0; j < kK; ++j) {
        codeword[j] = msg[j];
    }
    std::vector<uint8_t> parity(kM, 0);
    for (unsigned int r = 0; r < kM; ++r) {
        uint8_t acc = 0;
        for (unsigned int j = 0; j < kK; ++j) {
            acc ^= (h1[r][j] & msg[j]);
        }
        if (r > 0) {
            acc ^= parity[r - 1];
        }
        parity[r] = acc;
    }
    for (unsigned int r = 0; r < kM; ++r) {
        codeword[kK + r] = parity[r];
    }
    return codeword;
}

std::vector<uint8_t> decode_block(const std::vector<uint8_t>& received_block) {
    static const auto h = build_h();
    std::vector<uint8_t> r = received_block;

    for (unsigned int iter = 0; iter < kMaxIters; ++iter) {
        std::vector<uint8_t> syndrome(kM, 0);
        for (unsigned int i = 0; i < kM; ++i) {
            uint8_t s = 0;
            for (unsigned int j = 0; j < kN; ++j) {
                s ^= (h[i][j] & r[j]);
            }
            syndrome[i] = s;
        }
        if (std::all_of(syndrome.begin(), syndrome.end(), [](uint8_t s) { return s == 0; })) {
            break;
        }

        std::vector<unsigned int> unsatisfied_count(kN, 0);
        for (unsigned int j = 0; j < kN; ++j) {
            unsigned int count = 0;
            for (unsigned int i = 0; i < kM; ++i) {
                if (syndrome[i] && h[i][j]) {
                    ++count;
                }
            }
            unsatisfied_count[j] = count;
        }

        unsigned int max_count = *std::max_element(unsatisfied_count.begin(), unsatisfied_count.end());
        if (max_count == 0) {
            break;
        }
        for (unsigned int j = 0; j < kN; ++j) {
            if (unsatisfied_count[j] == max_count) {
                r[j] ^= 1;
            }
        }
    }

    return std::vector<uint8_t>(r.begin(), r.begin() + kK);
}

// Feature 3 (SIH26147 extra feature, soft-decision decoding): normalised min-sum
// belief propagation on the same parity-check matrix as decode_block, taking channel
// LLRs instead of hard bits. Standard sum-product/min-sum message passing:
//   variable -> check: this variable's current total LLR, minus what it last heard
//                       from this particular check (the "extrinsic" message)
//   check -> variable: the sign is the product of the other variables' signs, the
//                       magnitude the smallest of their magnitudes (min-sum: an
//                       approximation to the true sum-product update, much cheaper
//                       and the standard practical choice)
// A block "converges" once its hard decision satisfies every parity check, exactly
// the same stopping rule decode_block uses; kMaxIters otherwise bounds the work.
std::vector<uint8_t> decode_block_soft(const std::vector<float>& llrs) {
    static const auto h = build_h();
    // Per check, the connected variable indices (a fixed, small set: precomputed once).
    static const auto neighbors = [] {
        std::vector<std::vector<unsigned int>> nb(kM);
        for (unsigned int i = 0; i < kM; ++i)
            for (unsigned int j = 0; j < kN; ++j)
                if (h[i][j]) nb[i].push_back(j);
        return nb;
    }();

    std::vector<std::vector<float>> v2c(kM, std::vector<float>(kN, 0.0f));  // only entries in `neighbors` are used
    std::vector<float> total(llrs.begin(), llrs.end());

    for (unsigned int iter = 0; iter < kMaxIters; ++iter) {
        // check -> variable
        std::vector<std::vector<float>> c2v(kM, std::vector<float>(kN, 0.0f));
        for (unsigned int i = 0; i < kM; ++i) {
            for (unsigned int j : neighbors[i]) {
                float sign = 1.0f;
                float min_mag = std::numeric_limits<float>::infinity();
                for (unsigned int jj : neighbors[i]) {
                    if (jj == j) continue;
                    const float m = v2c[i][jj];
                    sign *= (m < 0.0f) ? -1.0f : 1.0f;
                    min_mag = std::min(min_mag, std::fabs(m));
                }
                c2v[i][j] = sign * min_mag;
            }
        }
        // variable update (total LLR), then the new extrinsic v2c for the next round
        for (unsigned int j = 0; j < kN; ++j) {
            total[j] = llrs[j];
            for (unsigned int i = 0; i < kM; ++i)
                if (h[i][j]) total[j] += c2v[i][j];
        }
        for (unsigned int i = 0; i < kM; ++i) {
            for (unsigned int j : neighbors[i]) v2c[i][j] = total[j] - c2v[i][j];
        }

        std::vector<uint8_t> hard(kN);
        for (unsigned int j = 0; j < kN; ++j) hard[j] = total[j] < 0.0f ? 1 : 0;
        bool satisfied = true;
        for (unsigned int i = 0; i < kM && satisfied; ++i) {
            uint8_t s = 0;
            for (unsigned int j : neighbors[i]) s ^= hard[j];
            satisfied = (s == 0);
        }
        if (satisfied) return std::vector<uint8_t>(hard.begin(), hard.begin() + kK);
    }

    std::vector<uint8_t> hard(kN);
    for (unsigned int j = 0; j < kN; ++j) hard[j] = total[j] < 0.0f ? 1 : 0;
    return std::vector<uint8_t>(hard.begin(), hard.begin() + kK);
}

}  // namespace

std::vector<uint8_t> ldpc_encode(const std::vector<uint8_t>& msg_bits) {
    if (msg_bits.empty()) {
        throw std::runtime_error("ldpc_encode: msg_bits is empty");
    }
    size_t num_blocks = (msg_bits.size() + kK - 1) / kK;
    std::vector<uint8_t> encoded(num_blocks * kN);

    for (size_t b = 0; b < num_blocks; ++b) {
        std::vector<uint8_t> block(kK, 0);
        for (unsigned int j = 0; j < kK; ++j) {
            size_t idx = b * kK + j;
            block[j] = (idx < msg_bits.size()) ? msg_bits[idx] : 0;
        }
        auto codeword = encode_block(block);
        std::copy(codeword.begin(), codeword.end(), encoded.begin() + b * kN);
    }
    return encoded;
}

std::vector<uint8_t> ldpc_decode(const std::vector<uint8_t>& encoded_bits, size_t msg_len_bits) {
    if (encoded_bits.empty()) {
        throw std::runtime_error("ldpc_decode: encoded_bits is empty");
    }
    if (encoded_bits.size() % kN != 0) {
        throw std::runtime_error("ldpc_decode: encoded_bits.size() must be a multiple of n=16");
    }
    size_t num_blocks = encoded_bits.size() / kN;
    std::vector<uint8_t> decoded(num_blocks * kK);

    for (size_t b = 0; b < num_blocks; ++b) {
        std::vector<uint8_t> block(encoded_bits.begin() + b * kN, encoded_bits.begin() + (b + 1) * kN);
        auto msg = decode_block(block);
        std::copy(msg.begin(), msg.end(), decoded.begin() + b * kK);
    }

    if (msg_len_bits > decoded.size()) {
        throw std::runtime_error("ldpc_decode: msg_len_bits exceeds decoded length");
    }
    decoded.resize(msg_len_bits);
    return decoded;
}

std::vector<uint8_t> ldpc_decode_soft(const std::vector<float>& llrs, size_t msg_len_bits) {
    if (llrs.empty()) {
        throw std::runtime_error("ldpc_decode_soft: llrs is empty");
    }
    if (llrs.size() % kN != 0) {
        throw std::runtime_error("ldpc_decode_soft: llrs.size() must be a multiple of n=16");
    }
    size_t num_blocks = llrs.size() / kN;
    std::vector<uint8_t> decoded(num_blocks * kK);
    for (size_t b = 0; b < num_blocks; ++b) {
        std::vector<float> block(llrs.begin() + static_cast<long>(b * kN), llrs.begin() + static_cast<long>((b + 1) * kN));
        auto msg = decode_block_soft(block);
        std::copy(msg.begin(), msg.end(), decoded.begin() + static_cast<long>(b * kK));
    }
    if (msg_len_bits > decoded.size()) {
        throw std::runtime_error("ldpc_decode_soft: msg_len_bits exceeds decoded length");
    }
    decoded.resize(msg_len_bits);
    return decoded;
}

}  // namespace sigcore
