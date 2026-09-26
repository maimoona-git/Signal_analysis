#pragma once

#include <cstdint>

namespace sigcore::gf256 {

// GF(256) arithmetic with primitive polynomial x^8 + x^4 + x^3 + x^2 + 1
// (0x11D) and primitive element alpha = 2. Used by the Reed-Solomon codec.

uint8_t add(uint8_t a, uint8_t b);  // == subtract, since char 2
uint8_t mul(uint8_t a, uint8_t b);
uint8_t div(uint8_t a, uint8_t b);  // b must be nonzero
uint8_t pow_alpha(int exponent);    // alpha^exponent, exponent may be negative
int log_alpha(uint8_t x);           // x must be nonzero; returns exponent such that alpha^e == x

}  // namespace sigcore::gf256
