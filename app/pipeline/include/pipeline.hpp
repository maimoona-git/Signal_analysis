#pragma once

// Port of app/pipeline.py: ingest -> DSP analysis -> ML classification ->
// demod -> de-interleave -> FEC decode -> bitstream correlation.
//
// The original is one synchronous function that calls the classifier in the
// middle. Here the classifier is asynchronous JS (onnxruntime-web), so the
// pipeline is split at exactly that point and the C++ stays synchronous:
//
//   PipelinePrep prep = pipeline_prepare(bytes, filename, options);   // ingest + spectrogram
//   ... classify (ML call-out, or a known label) -> Classification ...
//   PipelineResult r = pipeline_finish(std::move(prep), classification, options);
//
// sigbridge::run_pipeline_async (bridge/) does the ML step; run_pipeline() runs
// all of it given a classification, which is what the tests use.

#include <chrono>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "bits.hpp"
#include "channel_finder.hpp"
#include "correlate.hpp"
#include "demod_dispatch.hpp"
#include "framing.hpp"
#include "modulation_blind.hpp"
#include "interleave_dispatch.hpp"
#include "rank_analysis.hpp"
#include "raster.hpp"
#include "recover.hpp"
#include "signal_io.hpp"
#include "symbol_rate.hpp"

namespace sigapp {

// Time/frequency image of the capture for display (the waterfall): magnitude in
// dB, fft-shifted per frame, row-major (frames x nfft).
struct Spectrogram {
    std::vector<float> data;
    unsigned int frames = 0;
    unsigned int nfft = 0;
    std::vector<double> freqs;  // Hz, centred on 0; size nfft
    std::vector<double> times;  // seconds; size frames
};

// `hop` defaults to max(1, nfft / 4). Throws if the signal is shorter than nfft.
Spectrogram generate_spectrogram(const std::vector<std::complex<float>>& samples, double sample_rate,
                                 unsigned int nfft = 256, std::optional<unsigned int> hop = std::nullopt);

// The FFT size the pipeline picks for a capture: min(256, 2^(floor(log2 n) - 1)).
unsigned int pipeline_nfft(size_t num_samples);

// The original run_pipeline's keyword arguments. Interleaving/FEC are applied
// only if given; with `auto_detect` and neither given, detect_params picks them.
struct PipelineOptions {
    std::optional<double> sample_rate;        // required-in-spirit for raw .IQ (default 1 MHz)
    // How a raw .IQ/.sigmf-data file's samples are stored on disk -- ignored for .wav,
    // which carries its own PCM format in its header. See sigcore::IqSampleFormat's own
    // comment for what each one is and which real captures actually use it.
    sigcore::IqSampleFormat iq_format = sigcore::IqSampleFormat::Cf32;
    // Gap-list Phase 2.7: guess iq_format from the bytes (sigcore::guess_iq_format)
    // instead of trusting it. A SigMF datatype (below) still wins over a guess.
    bool iq_format_auto = false;
    // Gap-list Phase 2.6: the text of a `.sigmf-meta` sidecar, when the caller has one.
    // Supplies the sample rate (unless options.sample_rate is set), the sample format and
    // the center frequency (reported, not applied). See sigcore::parse_sigmf_meta.
    std::optional<std::string> sigmf_meta_json;
    unsigned int samples_per_symbol = 1;      // >1: naive decimation for linear schemes (not FSK)
    // Gap-list Phase 2.4: a non-integer true rate (e.g. 7.5 samples/symbol), when known or
    // estimated. For a linear scheme the capture is first resampled (cubic interpolation)
    // by round(x)/x so it sits at an integer rate, which then replaces samples_per_symbol.
    // Ignored for FSK, and when within 1% of an integer (nothing to fix).
    std::optional<double> samples_per_symbol_exact;

    // Gap-list Phase 4: bounded-window analysis of a raw .IQ/.sigmf-data capture. With
    // window_samples > 0 only samples [window_start_samples, window_start_samples +
    // window_samples) are parsed and analysed -- the rest of the file is never expanded to
    // complex floats, so memory is bounded by the window, not the file. 0 = the whole file.
    // (.wav is always read whole.) PipelineResult::file_total_samples/window_* say what ran.
    size_t window_start_samples = 0;
    size_t window_samples = 0;
    // Gap-list Phase 4 (streaming): when the caller has ALREADY sliced a big file down to the
    // bytes it hands in (the browser reads a Blob slice), these say where that slice sits in
    // the real file, so the result still reports "samples a..b of N" for the whole file.
    // 0 = the bytes given are the whole file.
    size_t source_total_bytes = 0;
    size_t source_start_byte = 0;
    DemodParams demod;                        // FSK parameters
    // Gap-list Phase 2.10: for an FSK capture, estimate tone count, symbol length and
    // bandwidth blind (sigcore::estimate_fsk_params) and use them instead of `demod`,
    // when the estimate is confident (>= 0.8); otherwise `demod` is used unchanged.
    bool fsk_auto = false;
    std::optional<InterleaverSpec> interleaver;
    std::optional<std::string> fec_scheme;
    std::optional<size_t> fec_msg_len;        // required when fec_scheme is given
    std::optional<Bits> header_pattern;       // default: the stream's first 16 bits
    bool auto_detect = false;

    // Feature 3 (SIH26147 extra feature): demodulate to LLRs and decode from them
    // instead of hard bits, when the modulation and FEC scheme support it (any linear
    // scheme, i.e. not FSK; "viterbi" or "ldpc", not "rs"/"concatenated" -- see
    // fec_dispatch.hpp's fec_supports_soft_decode). Silently falls back to the hard
    // path otherwise (PipelineResult::soft_decoding_used says which one actually ran),
    // so turning this on is always safe to try.
    bool soft_decoding = false;

    // Real-world signal support: for a linear scheme (not FSK) with samples_per_symbol
    // > 1, run sigcore::recover_linear (matched-filter timing recovery plus a
    // decision-directed carrier/phase tracking loop) instead of naive `[0, sps, 2*sps,
    // ...]` decimation, which only actually recovers a signal built with zero timing
    // offset and zero carrier error -- this project's own synthetic captures, not a
    // real recording. Off by default: it changes what gets decoded (a real capture's
    // recovered stream is not sample-identical to naive decimation, and a carrier loop
    // introduces its own, documented phase ambiguity -- see recover.hpp), so every
    // existing golden-vector test's behavior stays exactly as it was unless this is
    // turned on. PipelineResult::recovery_used says whether it actually ran.
    bool recover_timing = false;

    // Which transmit pulse shape recover_timing should assume (ignored unless
    // recover_timing is on): see sigcore::PulseShape's own comment. Defaults to Rrc,
    // the common case for real transmitter hardware.
    sigcore::PulseShape pulse_shape = sigcore::PulseShape::Rrc;
    float rolloff = 0.35f;  // RRC roll-off, used when pulse_shape is Rrc
    // Gap-list Phase 2.3: with auto_detect and recover_timing on, if the chosen pulse shape
    // does not give a verified decode, retry RRC at other roll-offs and Rect and keep the
    // first that verifies (PipelineResult::pulse_shape_searched). Set internally to stop
    // the retry recursing; callers leave it alone.
    bool pulse_search_done = false;
    // Internal (like `deadline`, not carried over the worker wire): run only the cheap link-layer
    // verifier (framing.hpp) and skip detect_params / Parameter Search. The pulse-shape search tries
    // every variant this way first, so the expensive FEC searches never eat the time budget before
    // a roll-off that a CRC can verify has even been tried.
    bool framing_only = false;

    // Gap-list Phase 2 safety bound: the automatic searches (Parameter Search, top-3
    // modulation retry, pulse/roll-off retry) multiply, and on a capture that simply is not
    // decodable they would otherwise run for minutes. After this many seconds from the
    // start of pipeline_finish no further search attempt is begun (the best result found
    // so far is returned). 0 = unbounded. `deadline` is filled in internally.
    double time_budget_sec = 8.0;
    std::optional<std::chrono::steady_clock::time_point> deadline;

    // Real-world signal support: for a wideband capture where the signal of interest
    // is not at baseband (see sigcore::find_channels's own comment -- the PS's own
    // stated case of a capture "recorded from different sensors and different
    // locations"), blind-detect candidate channels before anything else runs. Off by
    // default, like recover_timing: it is a real, separate mode, not something to
    // silently attempt on every capture. When on, results land in
    // PipelineResult::channel_candidates (sorted strongest first); this alone changes
    // nothing else about what gets decoded.
    bool find_channels_enabled = false;

    // Which of PipelineResult::channel_candidates to actually extract and process, by
    // index -- ignored unless find_channels_enabled is also on. A caller normally runs
    // once with find_channels_enabled alone to see the candidates, then again with
    // select_channel set once they know which one they want (the same "detect, then
    // choose" shape as Feature 1's Parameter Search, not a fully automatic pipeline).
    // When set to a valid index, sigcore::extract_channel's output REPLACES the raw
    // capture before the spectrogram, symbol-rate estimate, or demodulation ever see
    // it -- so, unlike recover_timing/pulse_shape, this changes what every other part
    // of the result reflects, not just how demodulation runs. See
    // PipelineResult::channel_extracted for whether it actually applied (false, not an
    // error, for an out-of-range index).
    std::optional<size_t> select_channel;

    // Gap-list Phase 2.5: with find_channels_enabled on and no select_channel given,
    // extract the strongest candidate automatically instead of waiting for a second,
    // manual step -- but only when it is narrower than most of the capture (a signal
    // filling the whole band has nothing to extract). PipelineResult::channel_auto_selected
    // says it happened; channel_used says which.
    bool auto_extract_channel = false;

    // Gap-list Phase 1.5: force the scheme used for demodulation/decode instead of
    // trusting the ML classifier's own top label (see Problem C -- the full
    // probability list is a separate concern, Phase 2.2's top-3 search; this is the
    // "I already know what this is" escape hatch for when the classifier is simply
    // wrong, or for testing). Any name demod_dispatch.hpp's modulate/demodulate
    // accept, case-insensitive, same as PipelineResult::predicted_modulation always
    // has been; nullopt (the default) leaves the classifier's own label in charge, as
    // always. When set, PipelineResult::predicted_modulation reports this scheme, not
    // the classifier's (what actually ran, not what the classifier guessed) --
    // predicted_confidence is left as the classifier's own number regardless, since
    // it is still a real, separate datum worth keeping visible.
    std::optional<std::string> modulation_override;
};

// Gap-list Phase 1.1: whether the final bits a caller sees are actually trustworthy,
// not just "whatever bits happened to be left after whichever stages ran" --
// `format_result_text`'s own "Decoded message" line used to print unconditionally,
// which on a capture with the wrong settings (no FEC applied, raw demodulated noise)
// showed a long hex string as if it were a real result. See DecodeStatus's own
// comment for exactly what each value means.
enum class DecodeStatus {
    // An FEC scheme was applied and reencode_agreement cleared 0.95 (the SAME
    // round-trip threshold detect_params itself already uses to accept a
    // candidate -- see detect.cpp's kMaxFecRoundTripBer -- kept consistent
    // deliberately, not a second, different number to tune separately).
    Verified,
    // No FEC verification available, but the frame raster (Feature 4) found a
    // real repeating frame structure with a sync word -- evidence the bits are
    // at least correctly framed, even without a decode to check them against.
    Structured,
    // Neither of the above: the bits may be right, may be noise -- nothing here
    // actually checked.
    Unverified,
};

// Gap-list Phase 1.3: where `PipelineResult::boundaries.header_pattern` actually came
// from -- the original app (and this port, until now) always used the stream's first
// 16 bits, whether or not that was anywhere close to the sync word the frame raster
// (Feature 4) itself finds from the data's own repetition. Reported so the panel can
// say plainly whether the header pattern is real evidence or just a guess.
enum class HeaderPatternSource {
    UserSpecified,  // options.header_pattern was given explicitly
    FrameRaster,    // no explicit pattern, but raster.detected -- raster.sync_word used
    Assumed,        // no explicit pattern and no raster detection -- first 16 bits, a guess
};

// Gap-list Phase 1.6: where PipelineResult::sample_rate actually came from -- Problem
// B's "1 MHz default IQ rate" is silent and easy to miss unless the panel says so
// outright. A .wav carries its own rate in its header regardless of options; a raw
// .IQ/.sigmf-data file has none, so it is either options.sample_rate (given
// explicitly) or signal_io.hpp's own kDefaultIqSampleRate guess.
enum class SampleRateSource {
    FromFileHeader,  // .wav: the file's own header, options.sample_rate is ignored
    UserSpecified,   // .iq/.sigmf-data with options.sample_rate given
    AssumedDefault,  // .iq/.sigmf-data with no options.sample_rate -- kDefaultIqSampleRate guessed
    FromSigmfMeta,   // the .sigmf-meta sidecar's core:sample_rate
};

// What the ML classifier decided.
struct Classification {
    std::string label;    // a modulation name, e.g. "bpsk", "fsk"
    double confidence = 0.0;
    // Gap-list Phase 2.2 (Problem C): the classifier's next-best labels, strongest
    // first (the caller passes at most the runner-up few). Empty = behave exactly as
    // before. With auto_detect on and the top label not producing a verified decode,
    // pipeline_finish retries each of these and keeps the first that verifies.
    std::vector<Classification> alternatives;
};

struct PipelinePrep {
    LoadedSignal signal;
    Spectrogram spectrogram;

    // Real-world signal support: see PipelineOptions::find_channels_enabled /
    // select_channel's own comments -- these mirror onto PipelineResult unchanged.
    std::vector<sigcore::ChannelCandidate> channel_candidates;
    bool channel_extracted = false;
    std::optional<size_t> channel_index;  // which candidate was extracted (explicit or automatic)
    bool channel_auto_selected = false;

    SampleRateSource sample_rate_source = SampleRateSource::AssumedDefault;
    size_t file_total_samples = 0;
    size_t window_start_samples = 0;
    std::optional<double> center_freq_hz;  // SigMF core:frequency, when a sidecar gave one
    bool iq_format_guessed = false;
    std::string iq_format_used;            // e.g. "cf32"; empty for .wav
};

struct PipelineResult {
    std::string file_path;
    double sample_rate = 0.0;
    size_t num_samples = 0;
    std::string file_format;
    std::vector<std::complex<float>> samples;
    Spectrogram spectrogram;
    std::string predicted_modulation;
    double predicted_confidence = 0.0;
    Bits demodulated_bits;
    std::optional<Bits> deinterleaved_bits;
    std::optional<Bits> decoded_bits;
    FrameBoundaries boundaries;
    HeaderPatternSource header_pattern_source = HeaderPatternSource::Assumed;
    std::optional<InterleaverSpec> detected_interleaver;
    std::optional<std::string> detected_fec;
    std::optional<int> detection_score;

    // Feature 4 (SIH26147 extra feature): the frame raster view, computed on the same
    // final bits as `boundaries`. `raster.detected` is false when no repeating frame
    // structure was found; the field is always present so the GUI can show that plainly.
    FrameRaster raster;

    // Feature 2 (SIH26147 extra feature): the rank-analysis view, computed BLIND on the
    // raw demodulated bits (before de-interleaving or FEC decode -- decoding removes the
    // very redundancy this looks for). `rank_analysis.detected` is false when no width's
    // rank deficiency clears the statistical floor; the field is always present so the
    // GUI can show that plainly, same convention as `raster`.
    RankAnalysis rank_analysis;

    // Feature 3 (SIH26147 extra feature): whether soft decoding actually ran (false if
    // it was not requested, or requested but not applicable -- FSK, or an FEC scheme
    // with no soft decoder). `reencode_agreement` is the universal "did the decode look
    // right" score (Section 2.3 of the feature spec): the decoded message, re-encoded,
    // compared bit for bit against what was actually received before FEC decoding
    // (matching_bits / total_bits); set whenever a fec_scheme was used, in either mode,
    // so it is directly comparable between them.
    bool soft_decoding_used = false;
    std::optional<double> reencode_agreement;

    // Whether PipelineOptions::recover_timing actually ran (false if it was not
    // requested, or requested but not applicable -- FSK, or samples_per_symbol <= 1,
    // where there is no timing offset to recover in the first place). When it did,
    // `recovery_phase_ambiguity` carries sigcore::RecoveryResult's own count, for the
    // GUI to explain (e.g. "2" for BPSK/ASK2/OOK means the decode may be a constant
    // bit-inversion of the true message -- see recover.hpp's own comment).
    bool recovery_used = false;
    std::optional<unsigned int> recovery_phase_ambiguity;

    // Real-world signal support: a blind, always-attempted suggestion for
    // samples_per_symbol, computed from the raw capture via
    // sigcore::estimate_symbol_rate before any modulation-specific demod runs
    // -- see that function's own comment for the technique and its measured
    // limits. Advisory only: it never overrides options.samples_per_symbol,
    // which stays a required, exact input (see that field's own comment) --
    // this is here so the GUI can show a suggested value next to it, not to
    // silently change what gets decoded. Left unset, not an error, when the
    // capture is too short for a usable estimate (see estimate_symbol_rate's
    // own minimum-length requirement).
    std::optional<sigcore::SymbolRateEstimate> symbol_rate_estimate;

    // Gap-list Phase 1.6: options.samples_per_symbol itself, copied onto the result
    // so the panel can report the value actually used for demodulation next to the
    // (purely advisory, never auto-applied at this layer) estimate just above.
    unsigned int samples_per_symbol_used = 1;
    std::optional<double> samples_per_symbol_fractional;  // set when the capture was resampled

    // Real-world signal support: see PipelineOptions::find_channels_enabled's own
    // comment. Empty when find_channels_enabled was off, or on but nothing cleared
    // the detection threshold -- either way, not an error.
    std::vector<sigcore::ChannelCandidate> channel_candidates;

    // Whether PipelineOptions::select_channel actually got applied -- false (not an
    // error) if it was not given, or given but out of range for channel_candidates.
    // When true, `samples`, `sample_rate`, `spectrogram` and `symbol_rate_estimate`
    // above all reflect the EXTRACTED channel, not the original wideband capture.
    bool channel_extracted = false;

    // Gap-list Phase 1.6: which candidate channel_extracted actually used -- set
    // together with it, same value as channel_candidates[*options.select_channel]
    // at the time this ran, kept here so the panel does not need options itself to
    // report it.
    std::optional<sigcore::ChannelCandidate> channel_used;
    bool channel_auto_selected = false;
    std::optional<size_t> channel_index;

    // Gap-list Phase 1.6: see SampleRateSource's own comment.
    SampleRateSource sample_rate_source = SampleRateSource::AssumedDefault;
    size_t file_total_samples = 0;     // samples in the whole file (raw IQ); 0 for .wav
    size_t window_start_samples = 0;   // where the analysed window began
    std::optional<double> center_freq_hz;
    bool iq_format_guessed = false;
    std::string iq_format_used;

    // Gap-list Phase 2.11: always-on, from the spectrogram alone: the strongest spectral
    // peak's offset from the capture's center (Hz) and the width of the contiguous band
    // around it within 10 dB of that peak. A coarse estimate (one FFT bin of resolution),
    // not a measurement of the true occupied bandwidth.
    std::optional<double> signal_offset_hz;
    std::optional<double> signal_bandwidth_hz;

    // Gap-list Phase 1.1: see DecodeStatus's own comment. Always computed, not
    // opt-in -- format_result_text's own `show_decode_status` flag controls
    // whether the TEXT reflects it (kept opt-in there, defaulting to false, so
    // the original app's panel text stays byte-identical unless a caller asks).
    DecodeStatus decode_status = DecodeStatus::Unverified;

    // Gap-list Phase 2.1: whether the automatic Parameter Search (not detect_params)
    // supplied the interleaver/FEC, and the bit offset it found.
    bool param_search_used = false;
    unsigned int param_search_bit_offset = 0;
    double param_search_confidence = 0.0;  // the winning candidate's search confidence, 0..1
    // Auto-detect, Parameter Search and the retries are skipped on very large captures (they cost
    // far more than the analysis itself); set when that happened to a run that asked for them.
    bool auto_search_skipped = false;

    // Link-layer framing (framing.hpp): CRC-valid AX.25 frames found in the demodulated stream
    // (auto-detect on). Finding one verifies the whole chain, so decode_status is Verified and
    // decoded_bits holds the first frame's information field.
    std::vector<Ax25Frame> frames;
    std::string framing_chain;

    // Set when the classifier said "fsk" but the signal failed the blind FSK tests (no tone
    // clusters, a varying envelope, a spectral line in its squared signal) and the pipeline
    // treated it as the linear family sigcore::blind_linear_check named instead.
    // classifier_top_label then says "fsk". blind_line is the line strength that decided it.
    bool modulation_from_blind_check = false;
    double blind_line = 0.0;
    double blind_envelope_cv = 0.0;

    // Gap-list Phase 2.2: set when an alternative (not the classifier's top) label was
    // the one that verified; classifier_top_label then says what the top guess was.
    bool modulation_from_search = false;
    std::string classifier_top_label;

    // Gap-list Phase 2.3: set when a different pulse shape / roll-off than requested verified.
    bool pulse_shape_searched = false;

    // Gap-list Phase 2.10: set when blind FSK estimates replaced options.demod.
    bool fsk_params_estimated = false;
    unsigned int fsk_bits_per_symbol = 0;
    unsigned int fsk_samples_per_symbol = 0;
    float fsk_bandwidth = 0.0f;
    double fsk_confidence = 0.0;
    std::string pulse_shape_used;  // "rrc 0.35" / "rect"; empty when no recovery ran
};

// Ingest and spectrogram. Throws (std::invalid_argument / std::runtime_error)
// for an unsupported extension, a malformed file, or a capture too short for the
// spectrogram.
PipelinePrep pipeline_prepare(const uint8_t* data, size_t size, const std::string& filename,
                              const PipelineOptions& options);

// Demod, de-interleave, decode, correlate. Throws std::invalid_argument for an
// unknown modulation label or a missing fec_msg_len, std::runtime_error from the
// core for uncorrectable / misshapen data.
PipelineResult pipeline_finish(PipelinePrep prep, const Classification& classification,
                               const PipelineOptions& options);

// prepare + finish with a known classification.
PipelineResult run_pipeline(const uint8_t* data, size_t size, const std::string& filename,
                            const PipelineOptions& options, const Classification& classification);

}  // namespace sigapp
