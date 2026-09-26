#include "pipeline.hpp"

#include <algorithm>
#include <stdexcept>

#include "detect.hpp"
#include "dsp.hpp"
#include "fec_dispatch.hpp"

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

    // Linear demodulators want one sample per symbol and there is no timing
    // recovery: decimate naively. FSK consumes the raw oversampled capture.
    std::vector<std::complex<float>> decimated;
    const std::vector<std::complex<float>>* samples_for_demod = &prep.signal.samples;
    if (scheme != "fsk" && options.samples_per_symbol > 1) {
        for (size_t i = 0; i < prep.signal.samples.size(); i += options.samples_per_symbol) {
            decimated.push_back(prep.signal.samples[i]);
        }
        samples_for_demod = &decimated;
    }

    PipelineResult result;
    result.demodulated_bits = demodulate(*samples_for_demod, scheme, options.demod);

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

    // NOTE (faithful to the original): after auto-detection the stream is
    // de-interleaved *again* here from the untrimmed demodulated bits, whereas
    // detect_params scored the convolutional candidate with its leading flush
    // bits removed. With a convolutional interleaver the two therefore differ.
    const Bits* working = &result.demodulated_bits;
    if (interleaver.has_value()) {
        result.deinterleaved_bits = deinterleave(*working, *interleaver);
        working = &*result.deinterleaved_bits;
    }

    if (fec_scheme.has_value()) {
        if (!fec_msg_len.has_value()) {
            throw std::invalid_argument("fec_msg_len is required when fec_scheme is given");
        }
        result.decoded_bits = fec_decode(*working, *fec_scheme, *fec_msg_len);
        working = &*result.decoded_bits;
    }

    result.boundaries = find_header_payload_boundaries(*working, options.header_pattern);

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
