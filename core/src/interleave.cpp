#include "interleave.hpp"

#include <algorithm>
#include <deque>
#include <numeric>
#include <random>
#include <stdexcept>

namespace sigcore {

namespace {

void check_matrix_size(const std::vector<uint8_t>& bits, unsigned int rows, unsigned int cols,
                        const char* who) {
    if (rows == 0 || cols == 0) {
        throw std::runtime_error(std::string(who) + ": rows and cols must be > 0");
    }
    if (bits.size() != static_cast<size_t>(rows) * cols) {
        throw std::runtime_error(std::string(who) + ": bits.size() must equal rows * cols");
    }
}

}  // namespace

std::vector<uint8_t> block_interleave(const std::vector<uint8_t>& bits, unsigned int rows,
                                       unsigned int cols) {
    check_matrix_size(bits, rows, cols, "block_interleave");
    std::vector<uint8_t> out(bits.size());
    for (unsigned int r = 0; r < rows; ++r) {
        for (unsigned int c = 0; c < cols; ++c) {
            out[static_cast<size_t>(c) * rows + r] = bits[static_cast<size_t>(r) * cols + c];
        }
    }
    return out;
}

std::vector<uint8_t> block_deinterleave(const std::vector<uint8_t>& bits, unsigned int rows,
                                         unsigned int cols) {
    check_matrix_size(bits, rows, cols, "block_deinterleave");
    std::vector<uint8_t> out(bits.size());
    for (unsigned int r = 0; r < rows; ++r) {
        for (unsigned int c = 0; c < cols; ++c) {
            out[static_cast<size_t>(r) * cols + c] = bits[static_cast<size_t>(c) * rows + r];
        }
    }
    return out;
}

std::vector<uint8_t> diagonal_interleave(const std::vector<uint8_t>& bits, unsigned int rows,
                                          unsigned int cols) {
    check_matrix_size(bits, rows, cols, "diagonal_interleave");
    // shifted[r][(c + r) % cols] = bits[r][c]
    std::vector<uint8_t> shifted(bits.size());
    for (unsigned int r = 0; r < rows; ++r) {
        for (unsigned int c = 0; c < cols; ++c) {
            unsigned int shifted_col = (c + r) % cols;
            shifted[static_cast<size_t>(r) * cols + shifted_col] = bits[static_cast<size_t>(r) * cols + c];
        }
    }
    // read out column-wise
    std::vector<uint8_t> out(bits.size());
    size_t k = 0;
    for (unsigned int c = 0; c < cols; ++c) {
        for (unsigned int r = 0; r < rows; ++r) {
            out[k++] = shifted[static_cast<size_t>(r) * cols + c];
        }
    }
    return out;
}

std::vector<uint8_t> diagonal_deinterleave(const std::vector<uint8_t>& bits, unsigned int rows,
                                            unsigned int cols) {
    check_matrix_size(bits, rows, cols, "diagonal_deinterleave");
    // undo the column-wise read: reconstruct shifted[r][c]
    std::vector<uint8_t> shifted(bits.size());
    size_t k = 0;
    for (unsigned int c = 0; c < cols; ++c) {
        for (unsigned int r = 0; r < rows; ++r) {
            shifted[static_cast<size_t>(r) * cols + c] = bits[k++];
        }
    }
    // undo the per-row cyclic shift: bits[r][c] = shifted[r][(c + r) % cols]
    std::vector<uint8_t> out(bits.size());
    for (unsigned int r = 0; r < rows; ++r) {
        for (unsigned int c = 0; c < cols; ++c) {
            unsigned int shifted_col = (c + r) % cols;
            out[static_cast<size_t>(r) * cols + c] = shifted[static_cast<size_t>(r) * cols + shifted_col];
        }
    }
    return out;
}

namespace {

std::vector<unsigned int> make_permutation(size_t n, unsigned int seed) {
    std::vector<unsigned int> perm(n);
    std::iota(perm.begin(), perm.end(), 0u);
    std::mt19937 rng(seed);
    std::shuffle(perm.begin(), perm.end(), rng);
    return perm;
}

}  // namespace

std::vector<uint8_t> pseudo_random_interleave(const std::vector<uint8_t>& bits,
                                               unsigned int seed) {
    if (bits.empty()) {
        throw std::runtime_error("pseudo_random_interleave: bits is empty");
    }
    auto perm = make_permutation(bits.size(), seed);
    std::vector<uint8_t> out(bits.size());
    for (size_t i = 0; i < bits.size(); ++i) {
        out[i] = bits[perm[i]];
    }
    return out;
}

std::vector<uint8_t> pseudo_random_deinterleave(const std::vector<uint8_t>& bits,
                                                 unsigned int seed) {
    if (bits.empty()) {
        throw std::runtime_error("pseudo_random_deinterleave: bits is empty");
    }
    auto perm = make_permutation(bits.size(), seed);
    std::vector<uint8_t> out(bits.size());
    for (size_t i = 0; i < bits.size(); ++i) {
        out[perm[i]] = bits[i];
    }
    return out;
}

namespace {

// Runs `branches` parallel FIFOs of length b * delay_increment (b = branch
// index), cycling the input across branches and emitting the popped value
// at each step. Used symmetrically for interleave (increasing delay) and
// deinterleave (decreasing delay).
std::vector<uint8_t> run_branch_fifos(const std::vector<uint8_t>& bits, unsigned int branches,
                                       unsigned int delay_increment,
                                       bool reverse_branch_order) {
    if (branches == 0) {
        throw std::runtime_error("convolutional interleaver: branches must be > 0");
    }
    if (bits.empty()) {
        throw std::runtime_error("convolutional interleaver: bits is empty");
    }

    std::vector<std::deque<uint8_t>> fifos(branches);
    for (unsigned int b = 0; b < branches; ++b) {
        unsigned int depth = reverse_branch_order ? (branches - 1 - b) * delay_increment
                                                   : b * delay_increment;
        fifos[b].assign(depth, 0);
    }

    std::vector<uint8_t> out(bits.size());
    for (size_t i = 0; i < bits.size(); ++i) {
        unsigned int branch = static_cast<unsigned int>(i % branches);
        fifos[branch].push_back(bits[i]);
        out[i] = fifos[branch].front();
        fifos[branch].pop_front();
    }
    return out;
}

}  // namespace

std::vector<uint8_t> convolutional_interleave(const std::vector<uint8_t>& bits,
                                               unsigned int branches,
                                               unsigned int delay_increment) {
    return run_branch_fifos(bits, branches, delay_increment, /*reverse_branch_order=*/false);
}

std::vector<uint8_t> convolutional_deinterleave(const std::vector<uint8_t>& bits,
                                                 unsigned int branches,
                                                 unsigned int delay_increment) {
    return run_branch_fifos(bits, branches, delay_increment, /*reverse_branch_order=*/true);
}

}  // namespace sigcore
