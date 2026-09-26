// Phase 3: de-interleaving round trips (block, diagonal, pseudo-random,
// convolutional) on the native and WASM builds, plus golden permutations.

#include <cstdint>
#include <vector>

#include "interleave.hpp"
#include "../harness.hpp"
#include "../testutil.hpp"

using namespace sigcore;
using namespace sigtest;

TEST_CASE("block interleaver round trip and actually permutes") {
    struct Dim { unsigned int r, c; };
    for (const Dim d : {Dim{4, 6}, Dim{8, 16}, Dim{1, 9}, Dim{9, 1}, Dim{7, 13}}) {
        const auto bits = random_bits(d.r * d.c, d.r * 100 + d.c);
        const auto il = block_interleave(bits, d.r, d.c);
        if (d.r > 1 && d.c > 1) CHECK(il != bits);
        CHECK(block_deinterleave(il, d.r, d.c) == bits);
    }
}

TEST_CASE("block interleaver writes rows, reads columns") {
    const std::vector<uint8_t> bits = {1, 2, 3, 4, 5, 6};  // 2x3 row-wise: [1 2 3; 4 5 6]
    CHECK(block_interleave(bits, 2, 3) == (std::vector<uint8_t>{1, 4, 2, 5, 3, 6}));
}

TEST_CASE("diagonal interleaver round trip and differs from block") {
    struct Dim { unsigned int r, c; };
    for (const Dim d : {Dim{5, 6}, Dim{8, 16}, Dim{3, 3}, Dim{2, 11}}) {
        const auto bits = random_bits(d.r * d.c, d.r * 31 + d.c);
        const auto il = diagonal_interleave(bits, d.r, d.c);
        CHECK(il != bits);
        CHECK(diagonal_deinterleave(il, d.r, d.c) == bits);
        CHECK(il != block_interleave(bits, d.r, d.c));
    }
}

TEST_CASE("pseudo-random interleaver round trip, seeds and sizes") {
    for (size_t n : {1u, 2u, 7u, 64u, 1000u}) {
        const auto bits = random_bits(n, n);
        const auto il = pseudo_random_interleave(bits, 1234);
        CHECK(pseudo_random_deinterleave(il, 1234) == bits);
    }
    const auto bits = random_bits(64, 7);
    CHECK(pseudo_random_interleave(bits, 1) != pseudo_random_interleave(bits, 2));
    CHECK(pseudo_random_interleave(bits, 5) == pseudo_random_interleave(bits, 5));  // deterministic
}

TEST_CASE("convolutional interleaver round trip after the flush delay") {
    struct P { unsigned int branches, delay; size_t n; };
    for (const P p : {P{4, 2, 64}, P{3, 1, 40}, P{5, 3, 200}, P{2, 4, 50}}) {
        const auto bits = random_bits(p.n, p.branches * 10 + p.delay);
        const auto il = convolutional_interleave(bits, p.branches, p.delay);
        const auto rec = convolutional_deinterleave(il, p.branches, p.delay);
        CHECK(rec.size() == bits.size());
        const size_t offset = static_cast<size_t>(p.branches) * (p.branches - 1) * p.delay;
        // First `offset` outputs are FIFO flush content; the rest is the stream, delayed.
        CHECK(std::equal(rec.begin() + static_cast<long>(offset), rec.end(), bits.begin()));
        for (size_t i = 0; i < offset; ++i) CHECK(rec[i] == 0);
    }
}

TEST_CASE("interleavers reject invalid input") {
    CHECK_THROWS(block_interleave(random_bits(10), 4, 4));
    CHECK_THROWS(block_deinterleave(random_bits(10), 4, 4));
    CHECK_THROWS(block_interleave(random_bits(0), 0, 4));
    CHECK_THROWS(diagonal_interleave(random_bits(10), 4, 4));
    CHECK_THROWS(pseudo_random_interleave({}, 1));
    CHECK_THROWS(pseudo_random_deinterleave({}, 1));
    CHECK_THROWS(convolutional_interleave(random_bits(8), 0, 1));
    CHECK_THROWS(convolutional_deinterleave({}, 4, 2));
}

// Golden hashes for a fixed 128-bit input (native run). The pseudo-random
// permutation comes from std::shuffle(std::mt19937), which the C++ standard
// leaves implementation-defined, so it is pinned explicitly.
TEST_CASE("golden interleaver output: block 8x16") {
    CHECK_EQ(fnv1a(block_interleave(random_bits(128, 42), 8, 16)), 1015550978);
}
TEST_CASE("golden interleaver output: diagonal 8x16") {
    CHECK_EQ(fnv1a(diagonal_interleave(random_bits(128, 42), 8, 16)), 301934173);
}
TEST_CASE("golden interleaver output: pseudo-random seed 1234") {
    CHECK_EQ(fnv1a(pseudo_random_interleave(random_bits(128, 42), 1234)), 1773352335);
}
TEST_CASE("golden interleaver output: convolutional 4x2") {
    CHECK_EQ(fnv1a(convolutional_interleave(random_bits(128, 42), 4, 2)), 520687930);
}

TEST_MAIN()
