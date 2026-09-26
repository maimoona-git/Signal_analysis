#pragma once

// Deterministic test data, identical on every platform: a hand-rolled
// xorshift generator (no <random> distributions, whose output is
// implementation-defined) and an FNV-1a hash for golden-vector checks.

#include <cstdint>
#include <vector>

namespace sigtest {

class Rng {
public:
    explicit Rng(uint64_t seed) : s_(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {}
    uint64_t next() {
        s_ ^= s_ << 13;
        s_ ^= s_ >> 7;
        s_ ^= s_ << 17;
        return s_;
    }
    // Uniform in [-1, 1).
    float uniform() { return static_cast<float>(next() >> 40) / 8388608.0f - 1.0f; }

private:
    uint64_t s_;
};

inline std::vector<uint8_t> random_bits(size_t n, uint64_t seed = 0) {
    Rng rng(seed);
    std::vector<uint8_t> v(n);
    for (auto& b : v) b = static_cast<uint8_t>((rng.next() >> 33) & 1);
    return v;
}

inline std::vector<uint8_t> random_bytes(size_t n, uint64_t seed = 0) {
    Rng rng(seed);
    std::vector<uint8_t> v(n);
    for (auto& b : v) b = static_cast<uint8_t>(rng.next() >> 33);
    return v;
}

// FNV-1a, folded to 31 bits so it prints/compares safely as a signed integer.
inline uint32_t fnv1a(const std::vector<uint8_t>& data) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint8_t b : data) {
        h ^= b;
        h *= 0x100000001b3ull;
    }
    return static_cast<uint32_t>(h ^ (h >> 32)) & 0x7fffffffu;
}

}  // namespace sigtest
