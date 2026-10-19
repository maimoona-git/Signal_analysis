#pragma once

// Gap-list Phase 5: ONE table of every FEC scheme this pipeline knows, replacing the
// parallel name lists and if-chains that used to live separately in fec_dispatch.cpp,
// detect.cpp and param_search.cpp (adding a scheme meant editing all three and they could
// drift). Each entry says how to encode/decode it, how to infer msg_len from an encoded
// length, how it behaves on pure noise, and how strict "verified" must be for it.

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "bits.hpp"

namespace sigapp {

struct FecKind {
    std::string name;
    bool msg_len_in_bytes = false;  // rs-family/concatenated count bytes; the rest count bits
    bool soft_decode = false;       // has a soft (LLR) decoder
    // re-encode agreement a real decode reaches vs. what undecodable noise reaches (both
    // measured, see param_search.cpp's note): `noise_baseline` normalises search
    // confidence, `verify_threshold` is the agreement a pipeline decode must clear to be
    // called Verified (kept clear of the highest noise agreement seen for the scheme).
    double noise_baseline = 0.9;
    double verify_threshold = 0.95;
    // param_search.hpp's alignment_margin a Verified decode must also clear. Punctured rate 2/3
    // barely distinguishes a shifted short stream (measured 0.010 on a genuine 297-bit message),
    // so it leans on its stricter verify_threshold instead.
    double min_alignment_margin = 0.03;
    // The four schemes detect_params (a faithful port, decision for decision) knows.
    // Newer schemes are searched by Parameter Search only.
    bool in_detect_params = false;
    std::function<Bits(const Bits&)> encode;
    std::function<Bits(const Bits&, size_t msg_len)> decode;
    // Message length implied by an encoded length, or nullopt if that length is invalid.
    std::function<std::optional<size_t>(size_t encoded_len)> msg_len_for;
};

const std::vector<FecKind>& fec_kinds();
// Case-insensitive; nullptr when unknown.
const FecKind* find_fec(const std::string& name);
// Names of the kinds with in_detect_params, in the order detect_params expects.
std::vector<std::string> fec_detect_kinds();

}  // namespace sigapp
