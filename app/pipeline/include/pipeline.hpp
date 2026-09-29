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

#include <complex>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "bits.hpp"
#include "correlate.hpp"
#include "demod_dispatch.hpp"
#include "interleave_dispatch.hpp"
#include "rank_analysis.hpp"
#include "raster.hpp"
#include "signal_io.hpp"

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
    unsigned int samples_per_symbol = 1;      // >1: naive decimation for linear schemes (not FSK)
    DemodParams demod;                        // FSK parameters
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
};

// What the ML classifier decided.
struct Classification {
    std::string label;    // a modulation name, e.g. "bpsk", "fsk"
    double confidence = 0.0;
};

struct PipelinePrep {
    LoadedSignal signal;
    Spectrogram spectrogram;
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
