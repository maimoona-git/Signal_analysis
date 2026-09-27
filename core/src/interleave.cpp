#include "interleave.hpp"

#include <algorithm>
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

std::vector<unsigned int> make_permutation(size_t n, unsigned int seed) {
    std::vector<unsigned int> perm(n);
    std::iota(perm.begin(), perm.end(), 0u);
    std::mt19937 rng(seed);
    std::shuffle(perm.begin(), perm.end(), rng);
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
