#pragma once

// Port of app/interleave.py: (de)interleaving by scheme *name* and parameters.

#include <cstdint>
#include <optional>
#include <string>

#include "bits.hpp"
#include "soft_demod.hpp"  // sigcore::LLRBuffer

namespace sigapp {

// One interleaver configuration. Which fields apply depends on `kind`:
//   block, diagonal : rows, cols
//   pseudo_random   : seed
//   convolutional   : branches, delay_increment
// (the original's `**params` dict; a missing required field is an error).
struct InterleaverSpec {
    std::string kind;
    std::optional<uint32_t> rows, cols, seed, branches, delay_increment;

    static InterleaverSpec block(uint32_t rows, uint32_t cols) { return {"block", rows, cols, {}, {}, {}}; }
    static InterleaverSpec diagonal(uint32_t rows, uint32_t cols) { return {"diagonal", rows, cols, {}, {}, {}}; }
    static InterleaverSpec pseudo_random(uint32_t seed) { return {"pseudo_random", {}, {}, seed, {}, {}}; }
    static InterleaverSpec convolutional(uint32_t branches, uint32_t delay_increment) {
        return {"convolutional", {}, {}, {}, branches, delay_increment};
    }

    bool operator==(const InterleaverSpec& o) const {
        return kind == o.kind && rows == o.rows && cols == o.cols && seed == o.seed &&
               branches == o.branches && delay_increment == o.delay_increment;
    }
};

// `kind` is case-insensitive. Throws std::invalid_argument for an unknown kind
// or a missing parameter; the sigcore layer throws std::runtime_error for bad
// sizes.
Bits interleave(const Bits& bits, const InterleaverSpec& spec);
Bits deinterleave(const Bits& bits, const InterleaverSpec& spec);

// Feature 3 (SIH26147 extra feature, soft-decision decoding): the same dispatch, over
// LLRs instead of hard bits -- every interleaver here is a fixed gather/permutation, so
// this moves confidences through exactly the same positions the bit version would.
sigcore::LLRBuffer interleave(const sigcore::LLRBuffer& llrs, const InterleaverSpec& spec);
sigcore::LLRBuffer deinterleave(const sigcore::LLRBuffer& llrs, const InterleaverSpec& spec);

}  // namespace sigapp
