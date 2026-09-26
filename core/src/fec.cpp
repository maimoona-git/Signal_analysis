#include "fec.hpp"

#include "reed_solomon.hpp"
#include "viterbi.hpp"

#include <algorithm>
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

}  // namespace sigcore
