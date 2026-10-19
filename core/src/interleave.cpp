#include "interleave.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <numeric>
#include <random>
#include <stdexcept>

namespace sigcore {

namespace {

template <class T>
void check_matrix_size(const std::vector<T>& bits, unsigned int rows, unsigned int cols, const char* who) {
    if (rows == 0 || cols == 0) {
        throw std::runtime_error(std::string(who) + ": rows and cols must be > 0");
    }
    if (bits.size() != static_cast<size_t>(rows) * cols) {
        throw std::runtime_error(std::string(who) + ": bits.size() must equal rows * cols");
    }
}

// Feature 3 (SIH26147 extra feature, soft-decision decoding): every interleaver here is
// a pure "gather from a fixed source position" operation -- which position feeds which
// output never depends on the DATA being routed, only on rows/cols/seed/branches/delay.
// So each is implemented once as a template and instantiated for both uint8_t (hard
// bits, the original behaviour, byte for byte) and float (LLRBuffer, soft values): the
// same permutation moves confidences exactly where it would move bits.

template <class T>
std::vector<T> block_interleave_t(const std::vector<T>& bits, unsigned int rows, unsigned int cols) {
    check_matrix_size(bits, rows, cols, "block_interleave");
    std::vector<T> out(bits.size());
    for (unsigned int r = 0; r < rows; ++r) {
        for (unsigned int c = 0; c < cols; ++c) {
            out[static_cast<size_t>(c) * rows + r] = bits[static_cast<size_t>(r) * cols + c];
        }
    }
    return out;
}

template <class T>
std::vector<T> block_deinterleave_t(const std::vector<T>& bits, unsigned int rows, unsigned int cols) {
    check_matrix_size(bits, rows, cols, "block_deinterleave");
    std::vector<T> out(bits.size());
    for (unsigned int r = 0; r < rows; ++r) {
        for (unsigned int c = 0; c < cols; ++c) {
            out[static_cast<size_t>(r) * cols + c] = bits[static_cast<size_t>(c) * rows + r];
        }
    }
    return out;
}

template <class T>
std::vector<T> diagonal_interleave_t(const std::vector<T>& bits, unsigned int rows, unsigned int cols) {
    check_matrix_size(bits, rows, cols, "diagonal_interleave");
    std::vector<T> shifted(bits.size());
    for (unsigned int r = 0; r < rows; ++r) {
        for (unsigned int c = 0; c < cols; ++c) {
            unsigned int shifted_col = (c + r) % cols;
            shifted[static_cast<size_t>(r) * cols + shifted_col] = bits[static_cast<size_t>(r) * cols + c];
        }
    }
    std::vector<T> out(bits.size());
    size_t k = 0;
    for (unsigned int c = 0; c < cols; ++c) {
        for (unsigned int r = 0; r < rows; ++r) {
            out[k++] = shifted[static_cast<size_t>(r) * cols + c];
        }
    }
    return out;
}

template <class T>
std::vector<T> diagonal_deinterleave_t(const std::vector<T>& bits, unsigned int rows, unsigned int cols) {
    check_matrix_size(bits, rows, cols, "diagonal_deinterleave");
    std::vector<T> shifted(bits.size());
    size_t k = 0;
    for (unsigned int c = 0; c < cols; ++c) {
        for (unsigned int r = 0; r < rows; ++r) {
            shifted[static_cast<size_t>(r) * cols + c] = bits[k++];
        }
    }
    std::vector<T> out(bits.size());
    for (unsigned int r = 0; r < rows; ++r) {
        for (unsigned int c = 0; c < cols; ++c) {
            unsigned int shifted_col = (c + r) % cols;
            out[static_cast<size_t>(r) * cols + c] = shifted[static_cast<size_t>(r) * cols + shifted_col];
        }
    }
    return out;
}

// floor(log2(x)) for x >= 1. A plain loop, not a compiler intrinsic
// (__builtin_clzll etc.): the whole point of this file is portability, so this
// avoids depending on yet another thing that is really a GCC/Clang extension,
// not standard C++. Cheap regardless -- called once per swap below, not a hot
// path at interleaver-setup time.
unsigned int floor_log2(uint64_t x) {
    unsigned int w = 0;
    while ((x >> (w + 1)) != 0) ++w;
    return w;
}

// Draws one uniformly-distributed index in [0, d] from raw std::mt19937 output,
// bit-for-bit reproducing what libc++'s std::shuffle(std::mt19937) actually
// computes internally (uniform_int_distribution<ptrdiff_t>(0, d), by way of its
// own __independent_bits_engine) -- see make_permutation's own comment for why
// matching libc++ specifically, not implementing shuffle some other equally
// valid way, is the point here.
//
// Traced directly from libc++'s own headers (__algorithm/shuffle.h,
// __random/uniform_int_distribution.h), not reconstructed from memory: for a
// 32-bit engine (mt19937's result_type) and a 64-bit working type (ptrdiff_t's
// unsigned counterpart, which is what this range ends up promoted to), the
// range size [0, d] always has exactly one "digit group" in libc++'s own
// generic bit-packing engine (true for any d small enough that log2(d+1) <=
// 32, i.e. any array size this project will ever actually interleave) -- which
// collapses that otherwise fairly general machinery down to exactly this: one
// raw engine draw, masked to the smallest number of bits that can represent
// every value in [0, d], retried on rejection (the draw lands in the part of
// that bit range that would overrepresent small values) until it lands inside
// [0, d] itself. Verified empirically against the real std::shuffle +
// std::mt19937 across sizes 0..4096 (every edge case around a power of two,
// where the retry logic actually matters) and several seeds, bit for bit.
unsigned int uniform_index(std::mt19937& rng, unsigned int d) {
    // d == 0: libc++'s uniform_int_distribution<ptrdiff_t>(0, 0) takes its
    // "only one possible value" fast path and returns 0 without drawing
    // anything at all -- not drawing here either keeps every later draw in
    // the same position in the mt19937 sequence as the original.
    if (d == 0) return 0;
    const uint64_t range_size = static_cast<uint64_t>(d) + 1;  // number of valid values in [0, d]
    unsigned int w = floor_log2(range_size);
    // range_size is not itself an exact power of two: one more bit is needed
    // to cover its own top value (e.g. range_size=9 needs 4 bits, not 3).
    if ((range_size & ((~0ULL) >> (64 - w))) != 0) ++w;
    const uint32_t mask = (w >= 32) ? 0xFFFFFFFFu : ((1u << w) - 1u);
    uint32_t draw;
    do {
        draw = rng() & mask;
    } while (draw >= range_size);
    return static_cast<unsigned int>(draw);
}

// A platform-independent reimplementation of std::shuffle(std::mt19937).
// std::mt19937 itself is fully specified by the standard (a fixed recurrence),
// so seeding it and drawing raw 32-bit values from it is already portable; it
// is std::shuffle's own index-mapping algorithm (by way of
// std::uniform_int_distribution) that the standard leaves unspecified --
// confirmed directly: built natively on this project with g++/libstdc++
// instead of Clang/libc++, the SAME seed produced a DIFFERENT permutation,
// which is exactly how two analysts' own builds of this tool could disagree on
// a pseudo-random-interleaved capture's decoded message, contradicting the
// "identical results for every analyst" claim.
//
// This reproduces libc++'s own specific algorithm (uniform_index above) bit
// for bit, chosen deliberately because that is what this project's existing
// golden fixtures -- recorded on this project's own native (Clang/libc++) and
// WASM (Emscripten, also libc++) builds, the only two platforms this was ever
// actually run on before -- already match, so this changes nothing about
// today's already-passing behavior while making it the same on every
// platform, not just these two.
std::vector<unsigned int> make_permutation(size_t n, unsigned int seed) {
    std::vector<unsigned int> perm(n);
    std::iota(perm.begin(), perm.end(), 0u);
    std::mt19937 rng(seed);
    if (n > 1) {
        for (size_t k = 0; k + 1 < n; ++k) {
            const unsigned int d = static_cast<unsigned int>(n - 1 - k);
            const unsigned int i = uniform_index(rng, d);
            if (i != 0) std::swap(perm[k], perm[k + i]);
        }
    }
    return perm;
}

template <class T>
std::vector<T> pseudo_random_interleave_t(const std::vector<T>& bits, unsigned int seed) {
    if (bits.empty()) {
        throw std::runtime_error("pseudo_random_interleave: bits is empty");
    }
    auto perm = make_permutation(bits.size(), seed);
    std::vector<T> out(bits.size());
    for (size_t i = 0; i < bits.size(); ++i) out[i] = bits[perm[i]];
    return out;
}

template <class T>
std::vector<T> pseudo_random_deinterleave_t(const std::vector<T>& bits, unsigned int seed) {
    if (bits.empty()) {
        throw std::runtime_error("pseudo_random_deinterleave: bits is empty");
    }
    auto perm = make_permutation(bits.size(), seed);
    std::vector<T> out(bits.size());
    for (size_t i = 0; i < bits.size(); ++i) out[perm[i]] = bits[i];
    return out;
}

// Runs `branches` parallel FIFOs of length b * delay_increment (b = branch index),
// cycling the input across branches and emitting the popped value at each step. Used
// symmetrically for interleave (increasing delay) and deinterleave (decreasing delay).
// The FIFOs' initial content is a default-constructed T (0 for both uint8_t and float),
// exactly like the original's zero-initialised flush prefix.
template <class T>
std::vector<T> run_branch_fifos_t(const std::vector<T>& bits, unsigned int branches, unsigned int delay_increment,
                                  bool reverse_branch_order) {
    if (branches == 0) {
        throw std::runtime_error("convolutional interleaver: branches must be > 0");
    }
    if (bits.empty()) {
        throw std::runtime_error("convolutional interleaver: bits is empty");
    }

    std::vector<std::deque<T>> fifos(branches);
    for (unsigned int b = 0; b < branches; ++b) {
        unsigned int depth = reverse_branch_order ? (branches - 1 - b) * delay_increment : b * delay_increment;
        fifos[b].assign(depth, T{});
    }

    std::vector<T> out(bits.size());
    for (size_t i = 0; i < bits.size(); ++i) {
        unsigned int branch = static_cast<unsigned int>(i % branches);
        fifos[branch].push_back(bits[i]);
        out[i] = fifos[branch].front();
        fifos[branch].pop_front();
    }
    return out;
}

}  // namespace

std::vector<uint8_t> block_interleave(const std::vector<uint8_t>& bits, unsigned int rows, unsigned int cols) {
    return block_interleave_t(bits, rows, cols);
}
std::vector<uint8_t> block_deinterleave(const std::vector<uint8_t>& bits, unsigned int rows, unsigned int cols) {
    return block_deinterleave_t(bits, rows, cols);
}
std::vector<float> block_interleave(const std::vector<float>& llrs, unsigned int rows, unsigned int cols) {
    return block_interleave_t(llrs, rows, cols);
}
std::vector<float> block_deinterleave(const std::vector<float>& llrs, unsigned int rows, unsigned int cols) {
    return block_deinterleave_t(llrs, rows, cols);
}

std::vector<uint8_t> diagonal_interleave(const std::vector<uint8_t>& bits, unsigned int rows, unsigned int cols) {
    return diagonal_interleave_t(bits, rows, cols);
}
std::vector<uint8_t> diagonal_deinterleave(const std::vector<uint8_t>& bits, unsigned int rows, unsigned int cols) {
    return diagonal_deinterleave_t(bits, rows, cols);
}
std::vector<float> diagonal_interleave(const std::vector<float>& llrs, unsigned int rows, unsigned int cols) {
    return diagonal_interleave_t(llrs, rows, cols);
}
std::vector<float> diagonal_deinterleave(const std::vector<float>& llrs, unsigned int rows, unsigned int cols) {
    return diagonal_deinterleave_t(llrs, rows, cols);
}

std::vector<uint8_t> pseudo_random_interleave(const std::vector<uint8_t>& bits, unsigned int seed) {
    return pseudo_random_interleave_t(bits, seed);
}
std::vector<uint8_t> pseudo_random_deinterleave(const std::vector<uint8_t>& bits, unsigned int seed) {
    return pseudo_random_deinterleave_t(bits, seed);
}
std::vector<float> pseudo_random_interleave(const std::vector<float>& llrs, unsigned int seed) {
    return pseudo_random_interleave_t(llrs, seed);
}
std::vector<float> pseudo_random_deinterleave(const std::vector<float>& llrs, unsigned int seed) {
    return pseudo_random_deinterleave_t(llrs, seed);
}

std::vector<uint8_t> convolutional_interleave(const std::vector<uint8_t>& bits, unsigned int branches,
                                              unsigned int delay_increment) {
    return run_branch_fifos_t(bits, branches, delay_increment, /*reverse_branch_order=*/false);
}
std::vector<uint8_t> convolutional_deinterleave(const std::vector<uint8_t>& bits, unsigned int branches,
                                                unsigned int delay_increment) {
    return run_branch_fifos_t(bits, branches, delay_increment, /*reverse_branch_order=*/true);
}
std::vector<float> convolutional_interleave(const std::vector<float>& llrs, unsigned int branches,
                                            unsigned int delay_increment) {
    return run_branch_fifos_t(llrs, branches, delay_increment, /*reverse_branch_order=*/false);
}
std::vector<float> convolutional_deinterleave(const std::vector<float>& llrs, unsigned int branches,
                                              unsigned int delay_increment) {
    return run_branch_fifos_t(llrs, branches, delay_increment, /*reverse_branch_order=*/true);
}

}  // namespace sigcore
