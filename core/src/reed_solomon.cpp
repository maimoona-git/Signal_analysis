#include "reed_solomon.hpp"

#include "gf256.hpp"

#include <algorithm>
#include <stdexcept>

namespace sigcore {

namespace {

constexpr size_t kN = 15;  // codeword length (bytes)
constexpr size_t kK = 13;  // message length (bytes)
constexpr size_t kT = 1;   // error-correcting capability
constexpr size_t kParity = kN - kK;  // = 2

// Generator polynomial g(x) = (x + alpha^1)(x + alpha^2)
//   = x^2 + (alpha^1 + alpha^2) x + alpha^3
// g0, g1 are the non-leading coefficients (g2 = 1 implied).
uint8_t g0() { return gf256::pow_alpha(3); }
uint8_t g1() { return gf256::add(gf256::pow_alpha(1), gf256::pow_alpha(2)); }

// Encodes one k-byte block into an n-byte systematic codeword:
// [message (k bytes, high-to-low) | parity (2 bytes)].
// Codeword coefficient layout (for syndrome purposes): c[i] is the
// coefficient of x^i, with message occupying x^kParity..x^(n-1) and parity
// occupying x^0..x^(kParity-1).
std::vector<uint8_t> encode_block(const std::vector<uint8_t>& msg) {
    uint8_t parity0 = 0, parity1 = 0;  // parity1 = coeff of x^1, parity0 = coeff of x^0
    for (size_t i = 0; i < kK; ++i) {
        uint8_t feedback = gf256::add(msg[i], parity1);
        parity1 = gf256::add(parity0, gf256::mul(feedback, g1()));
        parity0 = gf256::mul(feedback, g0());
    }

    std::vector<uint8_t> codeword(kN);
    for (size_t i = 0; i < kK; ++i) {
        codeword[i] = msg[i];
    }
    codeword[kK] = parity1;
    codeword[kK + 1] = parity0;
    return codeword;
}

// Syndrome S_j = C(alpha^j) evaluated over the codeword, where c[0] (the
// first message byte) is treated as the coefficient of x^(n-1) (highest
// degree) down to c[n-1] (second parity byte) as the coefficient of x^0 —
// i.e. the array is read in encode order, matching encode_block's layout.
uint8_t syndrome(const std::vector<uint8_t>& received, int j) {
    uint8_t alpha_j = gf256::pow_alpha(j);
    uint8_t s = 0;
    for (size_t idx = 0; idx < kN; ++idx) {
        s = gf256::add(gf256::mul(s, alpha_j), received[idx]);
    }
    return s;
}

// Decodes one n-byte block, correcting up to one byte error, returning the
// k-byte message.
std::vector<uint8_t> decode_block(const std::vector<uint8_t>& received) {
    uint8_t s1 = syndrome(received, 1);
    uint8_t s2 = syndrome(received, 2);

    std::vector<uint8_t> corrected = received;

    if (s1 != 0 || s2 != 0) {
        if (s1 == 0) {
            throw std::runtime_error("rs_decode: uncorrectable error pattern");
        }
        // Single-error case: S1 = e * alpha^i, S2 = e * alpha^(2i)
        // => alpha^i = S2 / S1  (i is the *coefficient* index from the high
        //    end, since received[0] is coeff of x^(n-1))
        uint8_t alpha_i = gf256::div(s2, s1);
        uint8_t e = gf256::div(s1, alpha_i);

        // received[idx] is the coefficient of x^(n-1-idx); find idx such
        // that alpha^(n-1-idx) == alpha_i.
        int power = gf256::log_alpha(alpha_i);
        int idx = static_cast<int>(kN) - 1 - power;
        if (idx < 0 || idx >= static_cast<int>(kN)) {
            throw std::runtime_error("rs_decode: uncorrectable error pattern");
        }
        corrected[idx] = gf256::add(corrected[idx], e);

        if (syndrome(corrected, 1) != 0 || syndrome(corrected, 2) != 0) {
            throw std::runtime_error("rs_decode: uncorrectable error pattern");
        }
    }

    return std::vector<uint8_t>(corrected.begin(), corrected.begin() + kK);
}

}  // namespace

std::vector<uint8_t> rs_encode_bytes(const std::vector<uint8_t>& msg) {
    if (msg.empty()) {
        throw std::runtime_error("rs_encode_bytes: msg is empty");
    }
    size_t num_blocks = (msg.size() + kK - 1) / kK;
    std::vector<uint8_t> encoded(num_blocks * kN);

    for (size_t b = 0; b < num_blocks; ++b) {
        std::vector<uint8_t> block(kK, 0);
        for (size_t j = 0; j < kK; ++j) {
            size_t idx = b * kK + j;
            block[j] = (idx < msg.size()) ? msg[idx] : 0;
        }
        auto codeword = encode_block(block);
        std::copy(codeword.begin(), codeword.end(), encoded.begin() + b * kN);
    }
    return encoded;
}

std::vector<uint8_t> rs_decode_bytes(const std::vector<uint8_t>& encoded, size_t msg_len) {
    if (encoded.empty()) {
        throw std::runtime_error("rs_decode_bytes: encoded is empty");
    }
    if (encoded.size() % kN != 0) {
        throw std::runtime_error("rs_decode_bytes: encoded.size() must be a multiple of n=15");
    }
    size_t num_blocks = encoded.size() / kN;
    std::vector<uint8_t> decoded(num_blocks * kK);

    for (size_t b = 0; b < num_blocks; ++b) {
        std::vector<uint8_t> block(encoded.begin() + b * kN, encoded.begin() + (b + 1) * kN);
        auto msg = decode_block(block);
        std::copy(msg.begin(), msg.end(), decoded.begin() + b * kK);
    }

    if (msg_len > decoded.size()) {
        throw std::runtime_error("rs_decode_bytes: msg_len exceeds decoded length");
    }
    decoded.resize(msg_len);
    return decoded;
}

}  // namespace sigcore
