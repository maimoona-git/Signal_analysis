#pragma once

// Feature 1 (SIH26147 extra feature, descoped): decode-driven parameter search. Not
// a full blind receiver front end (no timing recovery / pulse shaping search) --
// modulation and sample rate are still given, exactly as everywhere else in this
// pipeline -- but instead of `detect.hpp`'s single best-guess heuristic, this tries
// every (bit offset x interleaver x FEC) combination and ranks ALL of them by how
// much a decode actually looks *correct*, not by how often a header happens to
// repeat.
//
// Why this exists, not just detect_params: detect_params (deliberately, faithfully
// unchanged here -- it matches the original app decision for decision, see its own
// header) ranks candidates mainly by counting header repeats, with FEC round-tripping
// used only as a pass/fail filter. That is exactly right for a message that repeats
// several times, but a SINGLE message (no repeats at all, `header_repeat_score` is
// then just noise) gives it nothing reliable to rank on, and a real bug report during
// this port's own testing traced back to precisely that: detect_params picked "no
// FEC" over a genuinely-decodable LDPC hypothesis on a one-shot capture, because a
// coincidental header-repeat count beat it. This search instead ranks primarily by
// `reencode_agreement` (the SAME re-encode-and-compare metric Feature 3 already
// computes: how well the decoded message, re-encoded, matches what was actually
// received) -- a real correctness signal that does not need the message to repeat --
// and uses header repeats only as a secondary tie-break, and only for candidates
// that have no FEC hypothesis to check at all.
//
// Every candidate is kept (not just the winner), ranked, so the GUI can show the
// whole search as a ranked list ("a visible pruned tree with confidence", per the
// feature spec) and let a user pick a different one when the top guess is still
// wrong -- decode correctness is evidence, not proof.

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "bits.hpp"
#include "interleave_dispatch.hpp"

namespace sigapp {

struct ParamCandidate {
    unsigned bit_offset = 0;                    // bits dropped from the start before trying this candidate
    std::optional<InterleaverSpec> interleaver;  // nullopt = no interleaver
    std::optional<std::string> fec;              // nullopt = no FEC
    std::optional<size_t> fec_msg_len;
    // The re-encode-and-compare score (see the header comment), whenever `fec` is
    // set and decoding did not throw; nullopt for a no-FEC candidate, which has
    // nothing to check itself against.
    std::optional<double> reencode_agreement;
    int header_repeat_score = 0;
    // The combined ranking score this candidate was sorted by (see param_search.cpp):
    // `reencode_agreement` when present (a real correctness signal), otherwise a
    // capped fraction of header_repeat_score (weaker, and deliberately never allowed
    // to outrank a genuine decode elsewhere in the list).
    double confidence = 0.0;
    size_t decoded_bits = 0;  // the length of the candidate's final bits (post-FEC if any)
};

struct ParamSearchResult {
    // False when nothing cleared `score_floor`: `candidates` is still populated (the
    // whole search, for the GUI's ranked list), just none of it is claimed as "the"
    // answer.
    bool detected = false;
    // Index into `candidates` of the winner, when `detected`; candidates is sorted
    // by confidence descending, so this is 0 when detected.
    size_t best_index = 0;
    std::vector<ParamCandidate> candidates;  // every candidate tried, ranked
};

struct ParamSearchOptions {
    // Bound the work regardless of capture size, same reasoning as Feature 4/2's own
    // caps: a candidate is tried at real cost (interleave + FEC decode + re-encode),
    // and this search tries many of them.
    size_t max_analysis_bits = 200'000;
    // Bit offsets [0, max_bit_offset] to try (the "phase" of the feature spec): a
    // capture whose true frame boundary is not at bit 0 needs this to find anything.
    // 0 (the default) disables the offset search -- it multiplies the whole
    // candidate count by (max_bit_offset + 1), so it is opt-in.
    unsigned max_bit_offset = 0;
    unsigned pseudo_random_seed_range = 16;  // fewer than detect_params's 32: this is O(offsets) more work already
    // A confidence at or above this is a real find; below it, `detected` stays false
    // rather than reporting a low-confidence guess as the answer.
    double confidence_floor = 0.5;
    // The GUI shows the ranked list; nothing needs more than a modest page of it.
    size_t max_candidates_kept = 200;
};

// Throws std::invalid_argument only for empty input.
ParamSearchResult search_params(const Bits& bits, const ParamSearchOptions& options = {});

// Re-applies one candidate's parameters (bit offset, interleaver, FEC) to `bits` and
// returns its final decoded (or just de-interleaved, or just offset) bits -- exactly
// what that candidate's confidence was computed from, recomputed on demand so a
// caller (the GUI) can show the actual message for whichever candidate a user picks,
// without every candidate's full bits having to be kept in `ParamSearchResult`
// (a search can keep up to `max_candidates_kept`, and a capture can be sizeable, so
// that would add up fast). Throws std::invalid_argument for an offset at or past the
// end of `bits`, or for anything the original decode would already throw for.
Bits decode_candidate(const Bits& bits, const ParamCandidate& candidate);

}  // namespace sigapp
