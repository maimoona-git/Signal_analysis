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
    unsigned int samples_per_symbol = 1;      // >1: naive decimation for linear schemes (not FSK)
    DemodParams demod;                        // FSK parameters
    std::optional<InterleaverSpec> interleaver;
    std::optional<std::string> fec_scheme;
    std::optional<size_t> fec_msg_len;        // required when fec_scheme is given
    std::optional<Bits> header_pattern;       // default: the stream's first 16 bits
    bool auto_detect = false;
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
