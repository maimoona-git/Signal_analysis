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
    CHECK_THROWS(pseudo_random_interleave(std::vector<uint8_t>{}, 1));
    CHECK_THROWS(pseudo_random_deinterleave(std::vector<uint8_t>{}, 1));
    CHECK_THROWS(convolutional_interleave(random_bits(8), 0, 1));
    CHECK_THROWS(convolutional_deinterleave(std::vector<uint8_t>{}, 4, 2));
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

// Pins the pseudo-random permutation itself at a few (n, seed) pairs, readably
// (not just an opaque hash): make_permutation is no longer std::shuffle(std::
// mt19937) directly (an explicit, portable reimplementation replaced it -- see
// core/src/interleave.cpp's own comment on why: std::shuffle's exact algorithm
// is implementation-defined, so the old code silently produced a DIFFERENT
// permutation for the same seed on libstdc++ vs libc++, breaking the
// "identical results for every analyst" promise). These three were computed
// directly from the real std::shuffle(std::mt19937) on this project's own
// Clang/libc++ build (what every existing golden fixture already matches),
// confirmed bit-for-bit against the new explicit implementation before this
// test was written -- so this pins old (and still correct) behavior, not a
// new assumption.
//
// bits[i] = i % 2 is deliberately simple: pseudo_random_interleave's output
// at position i is bits[perm[i]] = perm[i] % 2 for this specific input, which
// makes the expected output directly readable off the permutation itself
// (listed in each comment) rather than needing it re-derived by a reader.
TEST_CASE("golden pseudo-random permutation, pinned directly (not just hashed)") {
    auto alternating = [](size_t n) {
        std::vector<uint8_t> bits(n);
        for (size_t i = 0; i < n; ++i) bits[i] = static_cast<uint8_t>(i % 2);
        return bits;
    };

    // perm = [4, 2, 0, 1, 3]
    const std::vector<uint8_t> expected5 = {0, 0, 0, 1, 1};
    CHECK(pseudo_random_interleave(alternating(5), 7) == expected5);
    // perm = [1, 4, 3, 2, 5, 0, 8, 7, 9, 6]
    const std::vector<uint8_t> expected10 = {1, 0, 1, 0, 1, 0, 0, 1, 1, 0};
    CHECK(pseudo_random_interleave(alternating(10), 99) == expected10);
    // perm = [8, 11, 2, 3, 15, 9, 13, 0, 12, 7, 5, 6, 10, 14, 1, 4]
    const std::vector<uint8_t> expected16 = {0, 1, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1, 0};
    CHECK(pseudo_random_interleave(alternating(16), 2024) == expected16);
}

TEST_MAIN()
