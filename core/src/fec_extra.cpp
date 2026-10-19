#include "fec_extra.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

#include "viterbi.hpp"

namespace sigcore {

namespace {

// Keep-mask over one puncturing period of the interleaved A,B stream.
std::vector<bool> puncture_mask(unsigned num, unsigned den) {
    if (num == 2 && den == 3) return {true, true, true, false};
    if (num == 3 && den == 4) return {true, true, false, true, true, false};
    throw std::invalid_argument("viterbi_punctured: supported rates are 2/3 and 3/4");
}

}  // namespace

size_t viterbi_punctured_length(size_t msg_len_bits, unsigned num, unsigned den) {
    const auto mask = puncture_mask(num, den);
    const size_t mother = 2 * (msg_len_bits + 6);
    size_t kept = 0;
    for (size_t i = 0; i < mother; ++i) kept += mask[i % mask.size()] ? 1 : 0;
    return kept;
}

std::vector<uint8_t> viterbi_punctured_encode(const std::vector<uint8_t>& msg_bits, unsigned num, unsigned den) {
    const auto mask = puncture_mask(num, den);
    const auto mother = viterbi_encode_bits(msg_bits);
    std::vector<uint8_t> out;
    out.reserve(mother.size());
    for (size_t i = 0; i < mother.size(); ++i)
        if (mask[i % mask.size()]) out.push_back(mother[i]);
    return out;
}

std::vector<uint8_t> viterbi_punctured_decode(const std::vector<uint8_t>& coded_bits, size_t msg_len_bits,
                                              unsigned num, unsigned den) {
    const auto mask = puncture_mask(num, den);
    const size_t mother = 2 * (msg_len_bits + 6);
    if (coded_bits.size() != viterbi_punctured_length(msg_len_bits, num, den)) {
        throw std::runtime_error("viterbi_punctured_decode: coded length does not match msg_len for this rate");
    }
    std::vector<float> llrs(mother, 0.0f);  // 0 = erasure: contributes nothing to any path metric
    size_t k = 0;
    for (size_t i = 0; i < mother; ++i)
        if (mask[i % mask.size()]) llrs[i] = coded_bits[k++] ? -1.0f : 1.0f;  // llr > 0 means bit 0
    return viterbi_decode_soft(llrs, msg_len_bits);
}

// ---- RS(255,223) ----------------------------------------------------------------------

namespace {

constexpr int kN = 255, kK = 223, kParity = kN - kK;

struct Gf {
    std::array<uint8_t, 512> exp{};
    std::array<int, 256> log{};
    Gf() {
        int x = 1;
        for (int i = 0; i < 255; ++i) {
            exp[i] = static_cast<uint8_t>(x);
            log[x] = i;
            x <<= 1;
            if (x & 0x100) x ^= 0x11D;
        }
        for (int i = 255; i < 512; ++i) exp[i] = exp[i - 255];
    }
    uint8_t mul(uint8_t a, uint8_t b) const { return (a && b) ? exp[log[a] + log[b]] : 0; }
    uint8_t div(uint8_t a, uint8_t b) const { return a ? exp[(log[a] + 255 - log[b]) % 255] : 0; }
    uint8_t inv(uint8_t a) const { return exp[255 - log[a]]; }
};
const Gf& gf() {
    static const Gf g;
    return g;
}

// Generator polynomial coefficients, highest degree first: prod_{i=1..32} (x - alpha^i).
std::vector<uint8_t> generator() {
    const Gf& g = gf();
    std::vector<uint8_t> poly = {1};
    for (int i = 1; i <= kParity; ++i) {
        std::vector<uint8_t> next(poly.size() + 1, 0);
        for (size_t j = 0; j < poly.size(); ++j) {
            next[j] ^= poly[j];
            next[j + 1] ^= g.mul(poly[j], g.exp[i]);
        }
        poly = std::move(next);
    }
    return poly;
}

std::vector<uint8_t> encode_block(const uint8_t* msg) {
    static const std::vector<uint8_t> gen = generator();
    const Gf& g = gf();
    std::vector<uint8_t> out(kN, 0);
    std::copy(msg, msg + kK, out.begin());
    for (int i = 0; i < kK; ++i) {
        const uint8_t coef = out[i];
        if (coef) {
            for (int j = 1; j <= kParity; ++j) out[i + j] ^= g.mul(gen[j], coef);
        }
    }
    std::copy(msg, msg + kK, out.begin());  // systematic: message back in front of the parity
    return out;
}

// Evaluate polynomial (highest degree first) at x.
uint8_t poly_eval(const std::vector<uint8_t>& p, uint8_t x) {
    const Gf& g = gf();
    uint8_t y = 0;
    for (uint8_t c : p) y = static_cast<uint8_t>(g.mul(y, x) ^ c);
    return y;
}

void decode_block(std::vector<uint8_t>& cw) {
    const Gf& g = gf();
    std::vector<uint8_t> synd(kParity);
    bool clean = true;
    for (int i = 0; i < kParity; ++i) {
        synd[i] = poly_eval(cw, g.exp[i + 1]);
        if (synd[i]) clean = false;
    }
    if (clean) return;

    // Berlekamp-Massey: error locator sigma (lowest degree first).
    std::vector<uint8_t> sigma = {1}, prev = {1};
    int L = 0, m = 1;
    uint8_t b = 1;
    for (int n = 0; n < kParity; ++n) {
        uint8_t d = synd[n];
        for (int i = 1; i <= L && i < static_cast<int>(sigma.size()); ++i) d ^= g.mul(sigma[i], synd[n - i]);
        if (d == 0) {
            ++m;
        } else if (2 * L <= n) {
            const auto t = sigma;
            sigma.resize(std::max(sigma.size(), prev.size() + m), 0);
            const uint8_t coef = g.div(d, b);
            for (size_t i = 0; i < prev.size(); ++i) sigma[i + m] ^= g.mul(coef, prev[i]);
            L = n + 1 - L;
            prev = t;
            b = d;
            m = 1;
        } else {
            sigma.resize(std::max(sigma.size(), prev.size() + m), 0);
            const uint8_t coef = g.div(d, b);
            for (size_t i = 0; i < prev.size(); ++i) sigma[i + m] ^= g.mul(coef, prev[i]);
            ++m;
        }
    }
    if (L > kParity / 2) throw std::runtime_error("rs255_decode: too many errors in a block");

    // Chien search: position p (index into cw) is in error if sigma(alpha^-(n-1-p)) == 0.
    std::vector<int> positions;
    for (int p = 0; p < kN; ++p) {
        const int power = kN - 1 - p;
        const uint8_t xinv = g.exp[(255 - power) % 255];
        uint8_t v = 0, xp = 1;
        for (size_t i = 0; i < sigma.size(); ++i) {
            v ^= g.mul(sigma[i], xp);
            xp = g.mul(xp, xinv);
        }
        if (v == 0) positions.push_back(p);
    }
    if (static_cast<int>(positions.size()) != L) throw std::runtime_error("rs255_decode: uncorrectable block");

    // Forney: omega = (S(x) * sigma(x)) mod x^32, with S(x) = sum synd[i] x^i.
    std::vector<uint8_t> omega(kParity, 0);
    for (int i = 0; i < kParity; ++i)
        for (size_t j = 0; j < sigma.size() && static_cast<int>(j) <= i; ++j) omega[i] ^= g.mul(synd[i - j], sigma[j]);
    for (int p : positions) {
        const int power = kN - 1 - p;
        const uint8_t x = g.exp[power % 255];
        const uint8_t xinv = g.inv(x);
        uint8_t num = 0, xp = 1;
        for (int i = 0; i < kParity; ++i) {
            num ^= g.mul(omega[i], xp);
            xp = g.mul(xp, xinv);
        }
        uint8_t den = 0;  // formal derivative of sigma at xinv: odd-degree terms only
        xp = 1;
        for (size_t i = 1; i < sigma.size(); i += 2) {
            uint8_t pw = 1;
            for (size_t k = 0; k + 1 < i; ++k) pw = g.mul(pw, xinv);
            den ^= g.mul(sigma[i], pw);
        }
        if (den == 0) throw std::runtime_error("rs255_decode: uncorrectable block");
        // With first consecutive root alpha^1 (fcr = 1) the X^(1-fcr) factor is 1:
        // e = omega(xinv) / sigma'(xinv)
        cw[p] ^= g.div(num, den);
    }
    for (int i = 0; i < kParity; ++i)
        if (poly_eval(cw, g.exp[i + 1]) != 0) throw std::runtime_error("rs255_decode: uncorrectable block");
}

}  // namespace

std::vector<uint8_t> rs255_encode_bytes(const std::vector<uint8_t>& msg) {
    std::vector<uint8_t> out;
    for (size_t pos = 0; pos < msg.size(); pos += kK) {
        std::vector<uint8_t> block(kK, 0);
        std::copy(msg.begin() + static_cast<long>(pos), msg.begin() + static_cast<long>(std::min(msg.size(), pos + kK)),
                  block.begin());
        const auto cw = encode_block(block.data());
        out.insert(out.end(), cw.begin(), cw.end());
    }
    return out;
}

std::vector<uint8_t> rs255_decode_bytes(const std::vector<uint8_t>& encoded, size_t msg_len) {
    if (encoded.size() % kN != 0) throw std::runtime_error("rs255_decode: length is not a multiple of 255 bytes");
    std::vector<uint8_t> out;
    for (size_t pos = 0; pos < encoded.size(); pos += kN) {
        std::vector<uint8_t> cw(encoded.begin() + static_cast<long>(pos), encoded.begin() + static_cast<long>(pos + kN));
        decode_block(cw);
        out.insert(out.end(), cw.begin(), cw.begin() + kK);
    }
    if (msg_len > out.size()) throw std::runtime_error("rs255_decode: msg_len exceeds the decoded data");
    out.resize(msg_len);
    return out;
}

}  // namespace sigcore
