#include "gf256.hpp"

#include <array>
#include <stdexcept>

namespace sigcore::gf256 {

namespace {

constexpr int kPrimitivePoly = 0x11D;

struct Tables {
    std::array<uint8_t, 510> exp{};  // extended to avoid modulo when multiplying
    std::array<int, 256> log{};

    Tables() {
        log[0] = -1;
        uint16_t x = 1;
        for (int i = 0; i < 255; ++i) {
            exp[i] = static_cast<uint8_t>(x);
            log[x] = i;
            x <<= 1;
            if (x & 0x100) {
                x ^= kPrimitivePoly;
            }
        }
        for (int i = 255; i < 510; ++i) {
            exp[i] = exp[i - 255];
        }
    }
};

const Tables& tables() {
    static const Tables t;
    return t;
}

}  // namespace

uint8_t add(uint8_t a, uint8_t b) { return a ^ b; }

uint8_t mul(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) return 0;
    const auto& t = tables();
    return t.exp[t.log[a] + t.log[b]];
}

uint8_t div(uint8_t a, uint8_t b) {
    if (b == 0) {
        throw std::runtime_error("gf256::div: division by zero");
    }
    if (a == 0) return 0;
    const auto& t = tables();
    return t.exp[t.log[a] + 255 - t.log[b]];
}

uint8_t pow_alpha(int exponent) {
    const auto& t = tables();
    int e = exponent % 255;
    if (e < 0) e += 255;
    return t.exp[e];
}

int log_alpha(uint8_t x) {
    if (x == 0) {
        throw std::runtime_error("gf256::log_alpha: log of zero is undefined");
    }
    return tables().log[x];
}

}  // namespace sigcore::gf256
