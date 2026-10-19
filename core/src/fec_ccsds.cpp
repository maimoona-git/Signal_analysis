#include "fec_ccsds.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

#include "viterbi.hpp"

namespace sigcore {

namespace {

constexpr int kN = 255;
constexpr int kK = 223;
constexpr int kParity = kN - kK;
constexpr int kFcr = 112;   // first consecutive root, as a power of beta
constexpr int kPrim = 11;   // beta = alpha^11

struct Field {
    std::array<uint8_t, 512> exp{};
    std::array<int, 256> log{};
    std::array<uint8_t, 256> to_dual{}, from_dual{};
    Field() {
        int x = 1;
        for (int i = 0; i < 255; ++i) {
            exp[i] = static_cast<uint8_t>(x);
            log[x] = i;
            x <<= 1;
            if (x & 0x100) x ^= 0x187;
        }
        for (int i = 255; i < 512; ++i) exp[i] = exp[i - 255];
        // Berlekamp dual basis (CCSDS 131.0-B annex): row k of the conversion matrix.
        static const uint8_t tal[8] = {0x8d, 0xef, 0xec, 0x86, 0xfa, 0x99, 0xaf, 0x7b};
        for (int v = 0; v < 256; ++v) {
            uint8_t d = 0;
            for (int j = 0; j < 8; ++j)
                for (int k = 0; k < 8; ++k)
                    if (v & (1 << k)) d ^= static_cast<uint8_t>(tal[7 - k] & (1 << j));
            to_dual[v] = d;
            from_dual[d] = static_cast<uint8_t>(v);
        }
    }
    uint8_t mul(uint8_t a, uint8_t b) const { return (a && b) ? exp[log[a] + log[b]] : 0; }
    uint8_t div(uint8_t a, uint8_t b) const { return a ? exp[(log[a] + 255 - log[b]) % 255] : 0; }
    uint8_t inv(uint8_t a) const { return exp[255 - log[a]]; }
    // beta^i, beta = alpha^11
    uint8_t beta(int i) const { return exp[((i % 255 + 255) % 255 * kPrim) % 255]; }
};
const Field& gf() {
    static const Field f;
    return f;
}

// prod_{i=0..31} (x - beta^(112+i)), highest degree first.
std::vector<uint8_t> generator() {
    const Field& g = gf();
    std::vector<uint8_t> poly = {1};
    for (int i = 0; i < kParity; ++i) {
        std::vector<uint8_t> next(poly.size() + 1, 0);
        for (size_t j = 0; j < poly.size(); ++j) {
            next[j] ^= poly[j];
            next[j + 1] ^= g.mul(poly[j], g.beta(kFcr + i));
        }
        poly = std::move(next);
    }
    return poly;
}

std::vector<uint8_t> encode_block(const uint8_t* msg) {
    static const std::vector<uint8_t> gen = generator();
    const Field& g = gf();
    std::vector<uint8_t> out(kN, 0);
    std::copy(msg, msg + kK, out.begin());
    for (int i = 0; i < kK; ++i) {
        const uint8_t coef = out[i];
        if (coef)
            for (int j = 1; j <= kParity; ++j) out[i + j] ^= g.mul(gen[j], coef);
    }
    std::copy(msg, msg + kK, out.begin());
    return out;
}

uint8_t poly_eval(const std::vector<uint8_t>& p, uint8_t x) {
    const Field& g = gf();
    uint8_t y = 0;
    for (uint8_t c : p) y = static_cast<uint8_t>(g.mul(y, x) ^ c);
    return y;
}

void decode_block(std::vector<uint8_t>& cw) {
    const Field& g = gf();
    std::vector<uint8_t> synd(kParity);
    bool clean = true;
    for (int i = 0; i < kParity; ++i) {
        synd[i] = poly_eval(cw, g.beta(kFcr + i));
        if (synd[i]) clean = false;
    }
    if (clean) return;

    // Berlekamp-Massey (error locator sigma, lowest degree first).
    std::vector<uint8_t> sigma = {1}, prev = {1};
    int L = 0, m = 1;
    uint8_t b = 1;
    for (int n = 0; n < kParity; ++n) {
        uint8_t d = synd[n];
        for (int i = 1; i <= L && i < static_cast<int>(sigma.size()); ++i) d ^= g.mul(sigma[i], synd[n - i]);
        if (d == 0) {
            ++m;
        } else {
            const auto t = sigma;
            sigma.resize(std::max(sigma.size(), prev.size() + m), 0);
            const uint8_t coef = g.div(d, b);
            for (size_t i = 0; i < prev.size(); ++i) sigma[i + m] ^= g.mul(coef, prev[i]);
            if (2 * L <= n) {
                L = n + 1 - L;
                prev = t;
                b = d;
                m = 1;
            } else {
                ++m;
            }
        }
    }
    if (L > kParity / 2) throw std::runtime_error("rs_ccsds_decode: too many errors in a block");

    // Chien search over locators X = beta^power.
    std::vector<int> positions;
    for (int p = 0; p < kN; ++p) {
        const int power = kN - 1 - p;
        const uint8_t xinv = g.beta(-power);
        uint8_t v = 0, xp = 1;
        for (size_t i = 0; i < sigma.size(); ++i) {
            v ^= g.mul(sigma[i], xp);
            xp = g.mul(xp, xinv);
        }
        if (v == 0) positions.push_back(p);
    }
    if (static_cast<int>(positions.size()) != L) throw std::runtime_error("rs_ccsds_decode: uncorrectable block");

    // Forney with first consecutive root beta^kFcr: e = X^(1-fcr) * omega(X^-1) / sigma'(X^-1).
    std::vector<uint8_t> omega(kParity, 0);
    for (int i = 0; i < kParity; ++i)
        for (size_t j = 0; j < sigma.size() && static_cast<int>(j) <= i; ++j) omega[i] ^= g.mul(synd[i - j], sigma[j]);
    for (int p : positions) {
        const int power = kN - 1 - p;
        const uint8_t xinv = g.beta(-power);
        uint8_t num = 0, xp = 1;
        for (int i = 0; i < kParity; ++i) {
            num ^= g.mul(omega[i], xp);
            xp = g.mul(xp, xinv);
        }
        uint8_t den = 0;
        for (size_t i = 1; i < sigma.size(); i += 2) {
            uint8_t pw = 1;
            for (size_t k = 0; k + 1 < i; ++k) pw = g.mul(pw, xinv);
            den ^= g.mul(sigma[i], pw);
        }
        if (den == 0) throw std::runtime_error("rs_ccsds_decode: uncorrectable block");
        cw[p] ^= g.mul(g.div(num, den), g.beta(power * (1 - kFcr)));
    }
    for (int i = 0; i < kParity; ++i)
        if (poly_eval(cw, g.beta(kFcr + i)) != 0) throw std::runtime_error("rs_ccsds_decode: uncorrectable block");
}

std::vector<uint8_t> to_bits(const std::vector<uint8_t>& bytes) {
    std::vector<uint8_t> bits;
    bits.reserve(bytes.size() * 8);
    for (uint8_t byte : bytes)
        for (int i = 7; i >= 0; --i) bits.push_back((byte >> i) & 1);
    return bits;
}

std::vector<uint8_t> to_bytes(const std::vector<uint8_t>& bits) {
    std::vector<uint8_t> bytes((bits.size() + 7) / 8, 0);
    for (size_t i = 0; i < bits.size(); ++i)
        if (bits[i]) bytes[i / 8] |= static_cast<uint8_t>(0x80 >> (i % 8));
    return bytes;
}

}  // namespace

std::vector<uint8_t> rs_ccsds_encode_bytes(const std::vector<uint8_t>& msg, bool dual_basis) {
    const Field& g = gf();
    std::vector<uint8_t> out;
    for (size_t pos = 0; pos < msg.size(); pos += kK) {
        std::vector<uint8_t> block(kK, 0);
        std::copy(msg.begin() + static_cast<long>(pos), msg.begin() + static_cast<long>(std::min(msg.size(), pos + kK)),
                  block.begin());
        if (dual_basis)
            for (auto& v : block) v = g.from_dual[v];
        auto cw = encode_block(block.data());
        if (dual_basis)
            for (auto& v : cw) v = g.to_dual[v];
        out.insert(out.end(), cw.begin(), cw.end());
    }
    return out;
}

std::vector<uint8_t> rs_ccsds_decode_bytes(const std::vector<uint8_t>& encoded, size_t msg_len, bool dual_basis) {
    const Field& g = gf();
    if (encoded.size() % kN != 0) throw std::runtime_error("rs_ccsds_decode: length is not a multiple of 255 bytes");
    std::vector<uint8_t> out;
    for (size_t pos = 0; pos < encoded.size(); pos += kN) {
        std::vector<uint8_t> cw(encoded.begin() + static_cast<long>(pos), encoded.begin() + static_cast<long>(pos + kN));
        if (dual_basis)
            for (auto& v : cw) v = g.from_dual[v];
        decode_block(cw);
        if (dual_basis)
            for (auto& v : cw) v = g.to_dual[v];
        out.insert(out.end(), cw.begin(), cw.begin() + kK);
    }
    if (msg_len > out.size()) throw std::runtime_error("rs_ccsds_decode: msg_len exceeds the decoded data");
    out.resize(msg_len);
    return out;
}

std::vector<uint8_t> ccsds_concat_encode(const std::vector<uint8_t>& msg_bytes) {
    if (msg_bytes.empty()) throw std::runtime_error("ccsds_concat_encode: msg is empty");
    return viterbi_encode_bits(to_bits(rs_ccsds_encode_bytes(msg_bytes)));
}

std::vector<uint8_t> ccsds_concat_decode(const std::vector<uint8_t>& coded_bits, size_t msg_len_bytes) {
    if (coded_bits.empty()) throw std::runtime_error("ccsds_concat_decode: coded_bits is empty");
    const size_t blocks = (msg_len_bytes + kK - 1) / kK;
    const auto rs_bits = viterbi_decode_bits(coded_bits, blocks * kN * 8);
    return rs_ccsds_decode_bytes(to_bytes(rs_bits), msg_len_bytes);
}

}  // namespace sigcore
