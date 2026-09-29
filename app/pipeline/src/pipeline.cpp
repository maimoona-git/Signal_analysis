#include "pipeline.hpp"

#include <algorithm>
#include <stdexcept>

#include "detect.hpp"
#include "dsp.hpp"
#include "fec_dispatch.hpp"
#include "recover.hpp"
#include "soft_demod.hpp"

namespace sigapp {

Spectrogram generate_spectrogram(const std::vector<std::complex<float>>& samples, double sample_rate,
                                 unsigned int nfft, std::optional<unsigned int> hop) {
    const unsigned int h = hop.value_or(std::max(1u, nfft / 4));
    Spectrogram s;
    s.nfft = nfft;
    s.data = sigcore::compute_spectrogram(samples, nfft, h, &s.frames);

    s.freqs.resize(nfft);
    for (unsigned int i = 0; i < nfft; ++i) {
        s.freqs[i] = (static_cast<double>(i) - static_cast<double>(nfft / 2)) * (sample_rate / nfft);
    }
    s.times.resize(s.frames);
    for (unsigned int i = 0; i < s.frames; ++i) {
        s.times[i] = static_cast<double>(i) * (static_cast<double>(h) / sample_rate);
    }
    return s;
}

unsigned int pipeline_nfft(size_t num_samples) {
    const size_t n = std::max<size_t>(2, num_samples);
    unsigned int log2n = 0;  // floor(log2(n))
    for (size_t v = n; v > 1; v >>= 1) ++log2n;
    const unsigned int shift = std::max(1, static_cast<int>(log2n) - 1);
    return std::min<unsigned int>(256, 1u << shift);
}

PipelinePrep pipeline_prepare(const uint8_t* data, size_t size, const std::string& filename,
                              const PipelineOptions& options) {
    PipelinePrep prep;
    prep.signal = load_signal(data, size, filename, options.sample_rate);
    prep.spectrogram = generate_spectrogram(prep.signal.samples, prep.signal.sample_rate,
                                            pipeline_nfft(prep.signal.num_samples));
    return prep;
}

PipelineResult pipeline_finish(PipelinePrep prep, const Classification& classification,
                               const PipelineOptions& options) {
    const std::string& scheme = classification.label;

    PipelineResult result;

    // Linear demodulators want one sample per symbol. Two ways to get there: naive
    // decimation (default -- exact on this project's own synthetic captures, which
    // have no timing offset or carrier error to begin with), or, opted into via
    // recover_timing, sigcore::recover_linear's matched-filter timing recovery plus
    // carrier tracking, for a real capture that actually has both. FSK consumes the
    // raw oversampled capture either way (see PipelineOptions::recover_timing's own
    // comment on why FSK is out of scope for now).
    std::vector<std::complex<float>> processed;
    const std::vector<std::complex<float>>* samples_for_demod = &prep.signal.samples;
    if (scheme != "fsk" && options.samples_per_symbol > 1) {
        if (options.recover_timing) {
            const auto recovered =
                sigcore::recover_linear(prep.signal.samples, options.samples_per_symbol, linear_scheme_from_name(scheme));
            processed = recovered.samples;
            result.recovery_used = true;
            result.recovery_phase_ambiguity = recovered.phase_ambiguity_count;
        } else {
            for (size_t i = 0; i < prep.signal.samples.size(); i += options.samples_per_symbol) {
                processed.push_back(prep.signal.samples[i]);
            }
        }
        samples_for_demod = &processed;
    }

    result.demodulated_bits = demodulate(*samples_for_demod, scheme, options.demod);
    // Feature 2 (SIH26147 extra feature): blind, before anything below tries to
    // de-interleave or decode it (both of which would remove exactly the redundancy
    // this looks for).
    result.rank_analysis = build_rank_analysis(result.demodulated_bits);

    std::optional<InterleaverSpec> interleaver = options.interleaver;
    std::optional<std::string> fec_scheme = options.fec_scheme;
    std::optional<size_t> fec_msg_len = options.fec_msg_len;

    if (options.auto_detect && !interleaver.has_value() && !fec_scheme.has_value()) {
        const DetectionResult detection = detect_params(result.demodulated_bits);
        interleaver = detection.interleaver;
        fec_scheme = detection.fec;
        fec_msg_len = detection.fec_msg_len;
        result.detected_interleaver = detection.interleaver;
        result.detected_fec = detection.fec;
        result.detection_score = detection.score;
    }

    // Feature 3 (SIH26147 extra feature): the soft path needs the LLRs from
    // demodulation, which only exists for linear schemes -- FSK has no soft demapper
    // here (see soft_demod.hpp). Silently degrades to the hard path otherwise; the
    // caller can tell which one ran from soft_decoding_used.
    const bool try_soft = options.soft_decoding && scheme != "fsk" && fec_scheme.has_value() &&
                         fec_supports_soft_decode(*fec_scheme);

    // NOTE (faithful to the original): after auto-detection the stream is
    // de-interleaved *again* here from the untrimmed demodulated bits, whereas
    // detect_params scored the convolutional candidate with its leading flush
    // bits removed. With a convolutional interleaver the two therefore differ.
    if (fec_scheme.has_value() && !fec_msg_len.has_value()) {
        throw std::invalid_argument("fec_msg_len is required when fec_scheme is given");
    }

    const Bits* working = &result.demodulated_bits;
    Bits pre_decode_hard_bits;  // for the re-encode agreement score below
    if (try_soft) {
        const sigcore::ModulationScheme mod_scheme = linear_scheme_from_name(scheme);
        const float sigma2 = sigcore::estimate_noise_variance_linear(*samples_for_demod, mod_scheme);
        sigcore::LLRBuffer llrs = sigcore::soft_demodulate_linear(*samples_for_demod, mod_scheme, sigma2);
        result.demodulated_bits = sigcore::llr_hard_decision(llrs);  // for display; matches the hard path exactly

        if (interleaver.has_value()) {
            llrs = deinterleave(llrs, *interleaver);
            result.deinterleaved_bits = sigcore::llr_hard_decision(llrs);
        }
        pre_decode_hard_bits = sigcore::llr_hard_decision(llrs);

        result.decoded_bits = fec_decode_soft(llrs, *fec_scheme, *fec_msg_len);
        result.soft_decoding_used = true;
        working = &*result.decoded_bits;
    } else {
        if (interleaver.has_value()) {
            result.deinterleaved_bits = deinterleave(*working, *interleaver);
            working = &*result.deinterleaved_bits;
        }

        if (fec_scheme.has_value()) {
            pre_decode_hard_bits = *working;
            result.decoded_bits = fec_decode(*working, *fec_scheme, *fec_msg_len);
            working = &*result.decoded_bits;
        }
    }

    if (result.decoded_bits.has_value()) {
        const Bits re_encoded = fec_encode(*result.decoded_bits, *fec_scheme);
        const size_t n = std::min(re_encoded.size(), pre_decode_hard_bits.size());
        size_t matches = 0;
        for (size_t i = 0; i < n; ++i) matches += (re_encoded[i] == pre_decode_hard_bits[i]);
        result.reencode_agreement = n > 0 ? static_cast<double>(matches) / n : 0.0;
    }

    result.boundaries = find_header_payload_boundaries(*working, options.header_pattern);
    // build_frame_raster only throws on empty input, which find_header_payload_boundaries
    // above would already have thrown on, so `*working` here is always non-empty.
    result.raster = build_frame_raster(*working);

    result.file_path = prep.signal.path;
    result.sample_rate = prep.signal.sample_rate;
    result.num_samples = prep.signal.num_samples;
    result.file_format = prep.signal.format;
    result.samples = std::move(prep.signal.samples);
    result.spectrogram = std::move(prep.spectrogram);
    result.predicted_modulation = classification.label;
    result.predicted_confidence = classification.confidence;
    return result;
}

PipelineResult run_pipeline(const uint8_t* data, size_t size, const std::string& filename,
                            const PipelineOptions& options, const Classification& classification) {
    return pipeline_finish(pipeline_prepare(data, size, filename, options), classification, options);
}

}  // namespace sigapp
