#include "pipeline.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

#include "detect.hpp"
#include "dsp.hpp"
#include "fec_dispatch.hpp"
#include "fec_registry.hpp"
#include "fsk_estimate.hpp"
#include "param_search.hpp"
#include "recover.hpp"
#include "sigmf_meta.hpp"
#include "soft_demod.hpp"

namespace sigapp {

namespace {

// Gap-list Phase 2.4: Catmull-Rom cubic resampling of a complex capture by `ratio`
// (output length ~ input length * ratio). Good enough at the >= 4 samples/symbol rates
// timing recovery needs anyway; not a general-purpose resampler.
std::vector<std::complex<float>> resample_cubic(const std::vector<std::complex<float>>& x, double ratio) {
    std::vector<std::complex<float>> y;
    if (x.size() < 4 || ratio <= 0.0) return x;
    const size_t out_n = static_cast<size_t>(static_cast<double>(x.size() - 3) * ratio);
    y.reserve(out_n);
    for (size_t k = 0; k < out_n; ++k) {
        const double pos = static_cast<double>(k) / ratio + 1.0;
        const size_t i = static_cast<size_t>(pos);
        const float t = static_cast<float>(pos - static_cast<double>(i));
        const auto p0 = x[i - 1], p1 = x[i], p2 = x[i + 1], p3 = x[i + 2];
        y.push_back(0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t * t +
                            (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t * t * t));
    }
    return y;
}

std::string printf_g(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

// Gap-list Phase 1.4: everything from demodulation through decode_status, as its own
// function -- see its one call site's own comment in pipeline_finish for why.
struct Analysis {
    Bits demodulated_bits;
    RankAnalysis rank_analysis;
    std::optional<InterleaverSpec> detected_interleaver;
    std::optional<std::string> detected_fec;
    std::optional<int> detection_score;
    std::optional<Bits> deinterleaved_bits;
    std::optional<Bits> decoded_bits;
    bool soft_decoding_used = false;
    std::optional<double> reencode_agreement;
    FrameRaster raster;
    HeaderPatternSource header_pattern_source = HeaderPatternSource::Assumed;
    FrameBoundaries boundaries;
    DecodeStatus decode_status = DecodeStatus::Unverified;
    bool param_search_used = false;
    unsigned int param_search_bit_offset = 0;
    double param_search_confidence = 0.0;
    bool auto_search_skipped = false;
    std::vector<Ax25Frame> frames;
    std::string framing_chain;
};

// Fraction of whole bytes that are printable ASCII (or \n \r \t): a crude readability score.
double printable_fraction(const Bits& bits) {
    const size_t n = bits.size() / 8;
    if (n == 0) return 0.0;
    size_t ok = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned c = 0;
        for (int b = 0; b < 8; ++b) c = (c << 1) | (bits[i * 8 + static_cast<size_t>(b)] ? 1u : 0u);
        if ((c >= 32 && c < 127) || c == '\n' || c == '\r' || c == '\t') ++ok;
    }
    return static_cast<double>(ok) / static_cast<double>(n);
}

// How much better a decode must be at its true alignment than shifted by a bit or three
// (param_search.hpp's alignment_margin) to count as verified: real coded data clears this with
// room to spare; a run-dominated, undecodable capture does not.
constexpr double kMinAlignmentMargin = 0.03;

// Auto-detect and Parameter Search try hundreds of interleaver x FEC combinations, each a decode
// of the whole stream: on a multi-million-bit stream one run took 46 s natively. Above these
// sizes they are skipped (and the result says so); a window or explicit settings still work.
constexpr size_t kMaxAutoSearchBits = 300'000;
constexpr size_t kMaxAutoSearchSamples = 2'000'000;
// Framing is a handful of linear passes (8 chains), so it can look at far more than the searches can.
// Between the largest linear value measured (6.2) and the smallest FSK value (8.5).
constexpr double kMaxSymbolRatePeakiness = 7.5;
constexpr size_t kMaxFramingBits = 4'000'000;

bool scheme_is_fsk_label(const std::string& label) { return label == "fsk" || label == "FSK"; }

Bits bytes_to_bits_msb(const std::vector<uint8_t>& bytes) {
    Bits bits(bytes.size() * 8);
    for (size_t i = 0; i < bytes.size(); ++i)
        for (int b = 0; b < 8; ++b) bits[i * 8 + static_cast<size_t>(b)] = (bytes[i] >> (7 - b)) & 1u;
    return bits;
}

// How well a demodulated stream looks like rate-1/2 convolutionally coded data at some small bit
// offset: the best re-encode agreement of a plain Viterbi decode over offsets 0..32. A cheap stand-in
// for "is this the right carrier rotation"; 0 when nothing decodes. Says nothing about other codes.
double rotation_probe_score(const Bits& bits) {
    const FecKind* kind = find_fec("viterbi");
    if (!kind || bits.size() > 20000) return 0.0;
    double best = 0.0;
    for (size_t offset = 0; offset <= 32 && offset + 16 < bits.size(); ++offset) {
        const Bits stream(bits.begin() + static_cast<long>(offset), bits.end());
        const auto msg_len = kind->msg_len_for(stream.size());
        if (!msg_len) continue;
        try {
            const Bits decoded = kind->decode(stream, *msg_len);
            const Bits re = kind->encode(decoded);
            const size_t n = std::min(re.size(), stream.size());
            size_t same = 0;
            for (size_t i = 0; i < n; ++i) same += (re[i] == stream[i]);
            if (n > 0) best = std::max(best, static_cast<double>(same) / static_cast<double>(n));
        } catch (const std::exception&) {
        }
    }
    return best;
}

Analysis analyze_samples(const std::vector<std::complex<float>>& samples_for_demod, const std::string& scheme,
                         const PipelineOptions& options, bool recovery_used) {
    Analysis a;
    a.demodulated_bits = demodulate(samples_for_demod, scheme, options.demod);
    // Feature 2 (SIH26147 extra feature): blind, before anything below tries to
    // de-interleave or decode it (both of which would remove exactly the redundancy
    // this looks for).
    a.rank_analysis = build_rank_analysis(a.demodulated_bits);

    std::optional<InterleaverSpec> interleaver = options.interleaver;
    std::optional<std::string> fec_scheme = options.fec_scheme;
    std::optional<size_t> fec_msg_len = options.fec_msg_len;

    // A CRC-valid AX.25 frame in the stream is the strongest verification there is (framing.hpp),
    // so look for one first; if found, the interleaver/FEC searches below have nothing to add.
    FramingResult framing;
    if (options.auto_detect && !interleaver.has_value() && !fec_scheme.has_value() &&
        a.demodulated_bits.size() <= kMaxFramingBits) {
        framing = find_ax25_frames(a.demodulated_bits);
    }
    const bool framing_ok = framing.found;

    const bool auto_wanted = options.auto_detect && !options.framing_only && !interleaver.has_value() &&
                             !fec_scheme.has_value() && !framing_ok;
    const bool auto_ok = a.demodulated_bits.size() <= kMaxAutoSearchBits;
    if (auto_wanted && !auto_ok) a.auto_search_skipped = true;
    std::optional<DetectionResult> detection;
    if (auto_wanted && auto_ok) {
        detection = detect_params(a.demodulated_bits);
        // With auto-detect now on by default, detect_params' header-repeat score alone can pick
        // an interleaver for data that has none (a single non-repeating message gives it noise to
        // rank): no FEC verified it, and the result is LESS readable than the raw bits. Trust the
        // raw stream then. (A real interleaver on repeating data makes the stream MORE readable,
        // so those cases are untouched.)
        if (!detection->fec && detection->interleaver && detection->deinterleaved_bits &&
            printable_fraction(a.demodulated_bits) > printable_fraction(*detection->deinterleaved_bits) + 0.2) {
            detection->interleaver.reset();
            detection->deinterleaved_bits.reset();
        }
        interleaver = detection->interleaver;
        fec_scheme = detection->fec;
        fec_msg_len = detection->fec_msg_len;
        a.detected_interleaver = detection->interleaver;
        a.detected_fec = detection->fec;
        a.detection_score = detection->score;
    }

    // Feature 3 (SIH26147 extra feature): the soft path needs the LLRs from
    // demodulation, which only exists for linear schemes -- FSK has no soft demapper
    // here (see soft_demod.hpp). Silently degrades to the hard path otherwise; the
    // caller can tell which one ran from soft_decoding_used.
    const bool try_soft = options.soft_decoding && scheme != "fsk" && fec_scheme.has_value() &&
                         fec_supports_soft_decode(*fec_scheme);

    if (fec_scheme.has_value() && !fec_msg_len.has_value()) {
        throw std::invalid_argument("fec_msg_len is required when fec_scheme is given");
    }

    const Bits* working = &a.demodulated_bits;
    Bits pre_decode_hard_bits;  // for the re-encode agreement score below
    if (try_soft) {
        const sigcore::ModulationScheme mod_scheme = linear_scheme_from_name(scheme);
        const float sigma2 = sigcore::estimate_noise_variance_linear(samples_for_demod, mod_scheme);
        sigcore::LLRBuffer llrs = sigcore::soft_demodulate_linear(samples_for_demod, mod_scheme, sigma2);
        a.demodulated_bits = sigcore::llr_hard_decision(llrs);  // for display; matches the hard path exactly

        if (interleaver.has_value()) {
            llrs = deinterleave(llrs, *interleaver);
            a.deinterleaved_bits = sigcore::llr_hard_decision(llrs);
        }
        pre_decode_hard_bits = sigcore::llr_hard_decision(llrs);

        a.decoded_bits = fec_decode_soft(llrs, *fec_scheme, *fec_msg_len);
        a.soft_decoding_used = true;
        working = &*a.decoded_bits;
    } else if (detection) {
        // Reuse detect_params' own already-correct results instead of re-deriving
        // them from the untrimmed demodulated bits: for a convolutional interleaver,
        // detect_params scores each candidate with its leading flush bits removed
        // (deinterleave()/fec_decode() themselves have no notion of "flush" -- that
        // trim is detect_params' own, separate step), so re-running deinterleave()
        // here from scratch on the untrimmed stream would disagree with what was
        // actually scored and picked, decoding wrongly even though the right
        // interleaver/FEC were correctly identified.
        if (interleaver.has_value()) {
            a.deinterleaved_bits = detection->deinterleaved_bits;
            working = a.deinterleaved_bits ? &*a.deinterleaved_bits : working;
        }
        pre_decode_hard_bits = a.deinterleaved_bits.value_or(a.demodulated_bits);
        if (fec_scheme.has_value()) {
            a.decoded_bits = detection->decoded_bits;
            working = a.decoded_bits ? &*a.decoded_bits : working;
        }
    } else {
        if (interleaver.has_value()) {
            a.deinterleaved_bits = deinterleave(*working, *interleaver);
            working = &*a.deinterleaved_bits;
        }

        if (fec_scheme.has_value()) {
            pre_decode_hard_bits = *working;
            a.decoded_bits = fec_decode(*working, *fec_scheme, *fec_msg_len);
            working = &*a.decoded_bits;
        }
    }

    if (framing_ok) {
        a.frames = framing.frames;
        a.framing_chain = framing.chain;
        const auto& first = framing.frames.front();
        a.decoded_bits = bytes_to_bits_msb(first.info.empty() ? first.raw : first.info);
        working = &*a.decoded_bits;
    }

    if (a.decoded_bits.has_value() && !framing_ok) {
        const Bits re_encoded = fec_encode(*a.decoded_bits, *fec_scheme);
        const size_t n = std::min(re_encoded.size(), pre_decode_hard_bits.size());
        size_t matches = 0;
        for (size_t i = 0; i < n; ++i) matches += (re_encoded[i] == pre_decode_hard_bits[i]);
        a.reencode_agreement = n > 0 ? static_cast<double>(matches) / n : 0.0;
    }

    // Gap-list Phase 2.1: detect_params ranks mainly by header repeats, so a single
    // non-repeating message (real traffic) is routinely missed. When auto-detect left
    // nothing verified, run the decode-driven Parameter Search (Feature 1) here and
    // adopt its winner only if it genuinely verifies -- it never replaces a verified
    // decode, and never makes an unverified one look verified. A recovered capture
    // also gets a bit-offset sweep, since matched-filter delay shifts the codeword start.
    // Each scheme has its own "verified" bar (fec_registry.hpp): the punctured/long codes
    // agree with pure noise at up to ~0.95, so a flat 0.95 would let noise verify.
    auto verify_bar = [](const std::optional<std::string>& fec) {
        const FecKind* k = fec ? find_fec(*fec) : nullptr;
        return k ? k->verify_threshold : 0.95;
    };
    auto margin_ok = [&](const std::optional<InterleaverSpec>& il, const std::optional<std::string>& fec,
                         const std::optional<size_t>& msg_len, unsigned offset) {
        if (!fec || !msg_len) return false;
        ParamCandidate pc;
        pc.bit_offset = offset;
        pc.interleaver = il;
        pc.fec = fec;
        pc.fec_msg_len = msg_len;
        const FecKind* kind = find_fec(*fec);
        return alignment_margin(a.demodulated_bits, pc) >= (kind ? kind->min_alignment_margin : kMinAlignmentMargin);
    };
    const bool verified_already = a.reencode_agreement &&
        *a.reencode_agreement >= verify_bar(a.detected_fec ? a.detected_fec : options.fec_scheme) &&
        margin_ok(interleaver, fec_scheme, fec_msg_len, 0);
    std::optional<size_t> used_msg_len = fec_msg_len;
    if (options.auto_detect && !options.framing_only && auto_ok && !framing_ok && !options.interleaver && !options.fec_scheme && !verified_already) {
        try {
            ParamSearchOptions so;
            so.deadline = options.deadline;
            if (recovery_used) so.max_bit_offset = 32;
            const ParamSearchResult sr = search_params(a.demodulated_bits, so);
            if (sr.detected) {
                const ParamCandidate& c = sr.candidates[sr.best_index];
                if (c.fec && c.reencode_agreement && *c.reencode_agreement >= verify_bar(c.fec) &&
                    margin_ok(c.interleaver, c.fec, c.fec_msg_len, c.bit_offset)) {
                    Bits final_bits = decode_candidate(a.demodulated_bits, c);
                    a.deinterleaved_bits.reset();
                    a.decoded_bits = std::move(final_bits);
                    a.detected_interleaver = c.interleaver;
                    a.detected_fec = c.fec;
                    a.detection_score = static_cast<int>(c.confidence * 100.0);
                    a.reencode_agreement = c.reencode_agreement;
                    a.param_search_used = true;
                    used_msg_len = c.fec_msg_len;
                    a.param_search_bit_offset = c.bit_offset;
                    a.param_search_confidence = c.confidence;
                    working = &*a.decoded_bits;
                }
            }
        } catch (const std::exception&) {
            // search is best-effort; keep the auto-detect result
        }
    }

    // build_frame_raster only throws on empty input, same as find_header_payload_boundaries
    // below, so `*working` here is always non-empty.
    a.raster = build_frame_raster(*working);

    // Gap-list Phase 1.3: use the raster's own found sync word as the header pattern
    // when one wasn't given explicitly and the raster actually found one -- real
    // evidence from this capture's own repetition, not the first-16-bits guess the
    // original app (and this port, until now) always used regardless of whether that
    // happened to mean anything. See HeaderPatternSource's own comment.
    std::optional<Bits> header_pattern = options.header_pattern;
    if (header_pattern.has_value()) {
        a.header_pattern_source = HeaderPatternSource::UserSpecified;
    } else if (a.raster.detected && !a.raster.sync_word.empty()) {
        header_pattern = a.raster.sync_word;
        a.header_pattern_source = HeaderPatternSource::FrameRaster;
    } else {
        a.header_pattern_source = HeaderPatternSource::Assumed;
    }
    a.boundaries = find_header_payload_boundaries(*working, header_pattern);

    // Gap-list Phase 1.1: see DecodeStatus's own comment. 0.95 matches detect.cpp's own
    // kMaxFecRoundTripBer (0.05 round-trip BER = 0.95 agreement) deliberately -- the
    // same threshold that already decides whether auto-detect accepts an FEC candidate
    // at all, not a second, independently-tuned number.
    const std::optional<std::string> used_fec = a.detected_fec ? a.detected_fec : options.fec_scheme;
    const FecKind* used_kind = used_fec ? find_fec(*used_fec) : nullptr;
    const double kVerifiedReencodeAgreement = used_kind ? used_kind->verify_threshold : 0.95;
    const bool aligned_ok = a.param_search_used
        ? margin_ok(a.detected_interleaver, a.detected_fec, used_msg_len, a.param_search_bit_offset)
        : margin_ok(interleaver, fec_scheme, fec_msg_len, 0);
    if (framing_ok || (aligned_ok && a.reencode_agreement && *a.reencode_agreement >= kVerifiedReencodeAgreement)) {
        a.decode_status = DecodeStatus::Verified;
    } else if (a.raster.detected && !a.raster.sync_word.empty()) {
        a.decode_status = DecodeStatus::Structured;
    } else {
        a.decode_status = DecodeStatus::Unverified;
    }
    return a;
}

}  // namespace

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
    std::optional<double> rate = options.sample_rate;
    sigcore::IqSampleFormat fmt = options.iq_format;
    bool from_meta_rate = false;
    if (options.sigmf_meta_json) {
        const sigcore::SigmfMeta meta = sigcore::parse_sigmf_meta(*options.sigmf_meta_json);
        if (!rate && meta.sample_rate) { rate = meta.sample_rate; from_meta_rate = true; }
        if (meta.format) fmt = *meta.format;
        prep.center_freq_hz = meta.center_freq_hz;
        if (meta.format) prep.iq_format_guessed = false;
    }
    if (options.iq_format_auto && !(options.sigmf_meta_json && sigcore::parse_sigmf_meta(*options.sigmf_meta_json).format)) {
        fmt = sigcore::guess_iq_format(data, size);
        prep.iq_format_guessed = true;
    }
    const uint8_t* load_data = data;
    size_t load_size = size;
    const bool is_wav_name = filename.size() >= 4 && (filename.compare(filename.size() - 4, 4, ".wav") == 0 ||
                                                       filename.compare(filename.size() - 4, 4, ".WAV") == 0);
    if (options.window_samples > 0 && !is_wav_name) {
        const size_t bps = fmt == sigcore::IqSampleFormat::Cf32 ? 8 : fmt == sigcore::IqSampleFormat::Ci16 ? 4 : 2;
        const size_t total = size / bps;
        const size_t start = std::min(options.window_start_samples, total);
        const size_t n = std::min(options.window_samples, total - start);
        prep.file_total_samples = total;
        prep.window_start_samples = start;
        load_data = data + start * bps;
        load_size = n * bps;
    }
    if (options.source_total_bytes > 0 && !is_wav_name) {
        const size_t bps = fmt == sigcore::IqSampleFormat::Cf32 ? 8 : fmt == sigcore::IqSampleFormat::Ci16 ? 4 : 2;
        prep.file_total_samples = options.source_total_bytes / bps;
        prep.window_start_samples = options.source_start_byte / bps + prep.window_start_samples;
    }
    prep.signal = load_signal(load_data, load_size, filename, rate, fmt);
    if (prep.signal.format.rfind("wav_", 0) != 0) {
        prep.iq_format_used = fmt == sigcore::IqSampleFormat::Ci16 ? "ci16" : fmt == sigcore::IqSampleFormat::Cu8 ? "cu8" : "cf32";
    } else {
        prep.iq_format_guessed = false;
    }

    // Gap-list Phase 1.6: see SampleRateSource's own comment. LoadedSignal::format
    // starts "wav_" only for a .wav file (wav_io.cpp); every other ingest path (raw
    // .IQ/.sigmf-data, iq_io.cpp) is where options.sample_rate matters at all.
    if (prep.signal.format.rfind("wav_", 0) == 0) {
        prep.sample_rate_source = SampleRateSource::FromFileHeader;
    } else if (from_meta_rate) {
        prep.sample_rate_source = SampleRateSource::FromSigmfMeta;
    } else if (options.sample_rate.has_value() && *options.sample_rate != 0.0) {
        prep.sample_rate_source = SampleRateSource::UserSpecified;
    } else {
        prep.sample_rate_source = SampleRateSource::AssumedDefault;
    }

    // Real-world signal support: before the spectrogram or anything downstream sees
    // the capture, optionally find and/or extract a channel -- see PipelineOptions::
    // find_channels_enabled/select_channel's own comments for why this has to happen
    // here, not in pipeline_finish. Best-effort: too short/degenerate a capture for
    // find_channels leaves channel_candidates empty, not a pipeline error.
    if (options.find_channels_enabled) {
        try {
            prep.channel_candidates = sigcore::find_channels(prep.signal.samples, prep.signal.sample_rate).channels;
        } catch (const std::runtime_error&) {
            // leave channel_candidates empty
        }
        std::optional<size_t> chosen = options.select_channel;
        if (!chosen && options.auto_extract_channel && !prep.channel_candidates.empty() &&
            prep.channel_candidates[0].bandwidth_hz < 0.6 * prep.signal.sample_rate) {
            chosen = 0;  // candidates are sorted strongest first
            prep.channel_auto_selected = true;
        }
        if (chosen && *chosen < prep.channel_candidates.size()) {
            prep.channel_index = *chosen;
            const auto extracted = sigcore::extract_channel(prep.signal.samples, prep.signal.sample_rate,
                                                             prep.channel_candidates[*chosen]);
            prep.signal.samples = std::move(extracted.samples);
            prep.signal.sample_rate = extracted.sample_rate;
            prep.signal.num_samples = prep.signal.samples.size();
            prep.signal.duration_sec =
                prep.signal.sample_rate > 0.0 ? static_cast<double>(prep.signal.num_samples) / prep.signal.sample_rate : 0.0;
            prep.channel_extracted = true;
        }
    }

    prep.spectrogram = generate_spectrogram(prep.signal.samples, prep.signal.sample_rate,
                                            pipeline_nfft(prep.signal.num_samples));
    return prep;
}

static PipelineResult pipeline_finish_impl(PipelinePrep prep, const Classification& classification,
                                           const PipelineOptions& options);

PipelineResult pipeline_finish(PipelinePrep prep, const Classification& classification,
                               const PipelineOptions& options) {
    PipelineOptions timed = options;
    if (!timed.deadline && timed.time_budget_sec > 0.0) {
        timed.deadline = std::chrono::steady_clock::now() +
                         std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                             std::chrono::duration<double>(timed.time_budget_sec));
    }
    return pipeline_finish_impl(std::move(prep), classification, timed);
}

static PipelineResult pipeline_finish_impl(PipelinePrep prep, const Classification& classification,
                                           const PipelineOptions& options) {
    // Constellation check on raw samples that are already one per symbol. Only when the spectrum is
    // flat, as symbol-rate linear data is: oversampled FSK (peakiness 8.5 and up, against 6.2 at most
    // for BPSK to QAM64) would otherwise be read as some PSK. The classifier's label is not used for
    // this, since "fsk" is exactly what it says for most of the linear signals it gets wrong.
    if (!prep.constellation_checked) {
        prep.constellation_checked = true;
        if (options.samples_per_symbol <= 1 && prep.signal.samples.size() >= 100 &&
            prep.signal.num_samples <= kMaxAutoSearchSamples && sigcore::spectral_peakiness(prep.signal.samples) < kMaxSymbolRatePeakiness) {
            const sigcore::ConstellationCheck cc = sigcore::constellation_check(prep.signal.samples);
            prep.constellation_label = cc.best;
            prep.constellation_c40 = cc.c40;
            prep.constellation_c42 = cc.c42;
        }
    }

    // The classifier's most common real-world mistake: calling a real BPSK recording "fsk" (all ten
    // CPSat recordings, 90-99% confidence). If the label is "fsk" but the raw samples fail the blind
    // FSK tests and show a linear family's signature (core/include/modulation_blind.hpp), analyse
    // it as that family. Only with auto-detect on, never against an explicit override.
    if (options.auto_detect && !options.modulation_override && scheme_is_fsk_label(classification.label) &&
        prep.signal.num_samples <= kMaxAutoSearchSamples) {
        const sigcore::BlindLinearCheck blind = sigcore::blind_linear_check(prep.signal.samples);
        if (!blind.family.empty()) {
            Classification switched = classification;
            switched.label = blind.family;
            PipelineResult r = pipeline_finish_impl(std::move(prep), switched, options);
            r.modulation_from_blind_check = true;
            r.classifier_top_label = classification.label;
            r.blind_envelope_cv = blind.envelope_cv;
            r.blind_line = blind.family == "bpsk" ? blind.line2 : blind.family == "qpsk" ? blind.line4 : blind.line8;
            return r;
        }
    }

    // Gap-list Phase 2.2: the classifier's top label alone decides everything below, so
    // a confident wrong top-1 used to sink the whole decode. If auto-detect is on and
    // the top label does not verify, retry the runner-ups and keep the first that does.
    const bool small_enough_to_retry = prep.signal.num_samples <= kMaxAutoSearchSamples;
    if (options.auto_detect && small_enough_to_retry && !options.modulation_override && !options.interleaver &&
        !options.fec_scheme && !classification.alternatives.empty()) {
        Classification top = classification;
        top.alternatives.clear();
        PipelineResult best = pipeline_finish_impl(prep, top, options);
        if (best.decode_status != DecodeStatus::Verified) {
            // Several wrong schemes can still "verify": demodulating a BPSK capture as
            // 8-PSK just repeats each real bit, and FEC happily decodes that. A lower-order
            // scheme demodulating a higher-order signal cannot do the reverse, so among
            // everything that verifies, prefer the one yielding the fewest bits per sample
            // (Occam), not merely the classifier's runner-up order.
            std::optional<PipelineResult> chosen;
            // The constellation check's reading goes first when it disagrees with the label just tried.
            std::vector<Classification> order = classification.alternatives;
            const bool have_cc = !best.constellation_label.empty() && best.constellation_label != top.label;
            if (have_cc) {
                order.erase(std::remove_if(order.begin(), order.end(),
                                           [&](const Classification& c) { return c.label == best.constellation_label; }),
                            order.end());
                order.insert(order.begin(), Classification{best.constellation_label, 0.0, {}});
            }
            for (const auto& alt : order) {
                if (options.deadline && std::chrono::steady_clock::now() > *options.deadline) break;
                try {
                    Classification c{alt.label, alt.confidence, {}};
                    PipelineResult cand = pipeline_finish_impl(prep, c, options);
                    if (cand.decode_status == DecodeStatus::Verified &&
                        (!chosen || cand.demodulated_bits.size() < chosen->demodulated_bits.size())) {
                        chosen = std::move(cand);
                    }
                } catch (const std::exception&) {
                    // a wrong guess may legitimately fail to demodulate; try the next one
                }
            }
            if (chosen) {
                chosen->modulation_from_search = true;
                chosen->classifier_top_label = classification.label;
                chosen->modulation_from_constellation = have_cc && chosen->predicted_modulation == best.constellation_label;
                return *chosen;
            }
        }
        return best;
    }

    // Gap-list Phase 1.5: see PipelineOptions::modulation_override's own comment.
    const std::string& scheme = options.modulation_override ? *options.modulation_override : classification.label;

    // Gap-list Phase 2.4: bring a fractional true rate onto an integer one first.
    if (options.samples_per_symbol_exact && scheme != "fsk") {
        const double exact = *options.samples_per_symbol_exact;
        const double rounded = std::round(exact);
        if (exact >= 2.0 && rounded >= 2.0 && std::fabs(exact - rounded) > 0.01 * rounded) {
            PipelinePrep resampled = std::move(prep);
            resampled.signal.samples = resample_cubic(resampled.signal.samples, rounded / exact);
            resampled.signal.num_samples = resampled.signal.samples.size();
            PipelineOptions eff = options;
            eff.samples_per_symbol_exact.reset();
            eff.samples_per_symbol = static_cast<unsigned int>(rounded);
            PipelineResult r = pipeline_finish_impl(std::move(resampled), classification, eff);
            r.samples_per_symbol_fractional = exact;
            return r;
        }
    }

    // Gap-list Phase 2.10: blind FSK parameters, applied only when confident.
    if (options.fsk_auto && scheme == "fsk") {
        try {
            const sigcore::FskEstimate est = sigcore::estimate_fsk_params(prep.signal.samples);
            if (est.confidence >= 0.8 && est.samples_per_symbol > 0) {
                PipelineOptions eff = options;
                eff.fsk_auto = false;
                eff.demod.bits_per_symbol = est.bits_per_symbol;
                eff.demod.samples_per_symbol = est.samples_per_symbol;
                eff.demod.bandwidth = est.bandwidth;
                PipelineResult r = pipeline_finish_impl(std::move(prep), classification, eff);
                r.fsk_params_estimated = true;
                r.fsk_bits_per_symbol = est.bits_per_symbol;
                r.fsk_samples_per_symbol = est.samples_per_symbol;
                r.fsk_bandwidth = est.bandwidth;
                r.fsk_confidence = est.confidence;
                return r;
            }
        } catch (const std::exception&) {
            // too short or degenerate: keep the configured FSK parameters
        }
    }

    // Gap-list Phase 2.3: an unverified decode through matched-filter recovery may just
    // have the wrong pulse model. Try the requested one first, then other roll-offs and
    // Rect, and keep the first that verifies; otherwise keep the requested one's result.
    if (options.auto_detect && small_enough_to_retry && options.recover_timing && !options.pulse_search_done && !options.interleaver &&
        !options.fec_scheme && options.samples_per_symbol > 1 && scheme != "fsk") {
        PipelineOptions base = options;
        base.pulse_search_done = true;

        struct Variant { sigcore::PulseShape shape; float rolloff; };
        // 0.75 and 1.0 were added after real recordings (the CPSat G3RUH beacons) came out
        // bit-perfect only at 0.75: a wide, heavily filtered pulse is common on real links.
        const Variant variants[] = {{sigcore::PulseShape::Rrc, 0.35f}, {sigcore::PulseShape::Rrc, 0.25f},
                                    {sigcore::PulseShape::Rrc, 0.5f},  {sigcore::PulseShape::Rrc, 0.75f},
                                    {sigcore::PulseShape::Rrc, 1.0f},  {sigcore::PulseShape::Rect, 0.35f}};

        // Pass 1, cheap: the requested pulse first, then every variant, each with ONLY the link-layer
        // verifier (a recovery, a demodulation and a CRC check -- milliseconds). A CRC-valid frame
        // settles which pulse model is right without any FEC search.
        {
            PipelineOptions cheap = base;
            cheap.framing_only = true;
            auto try_cheap = [&](sigcore::PulseShape shape, float rolloff, bool searched) -> std::optional<PipelineResult> {
                PipelineOptions alt = cheap;
                alt.pulse_shape = shape;
                alt.rolloff = rolloff;
                try {
                    PipelineResult cand = pipeline_finish_impl(prep, classification, alt);
                    if (cand.decode_status == DecodeStatus::Verified) {
                        cand.pulse_shape_searched = searched;
                        return cand;
                    }
                } catch (const std::exception&) {
                    // a bad pulse model can fail outright; try the next
                }
                return std::nullopt;
            };
            if (auto hit = try_cheap(options.pulse_shape, options.rolloff, false)) return *hit;
            for (const Variant& v : variants) {
                const bool same = v.shape == options.pulse_shape &&
                                  (v.shape == sigcore::PulseShape::Rect || v.rolloff == options.rolloff);
                if (same) continue;
                if (auto hit = try_cheap(v.shape, v.rolloff, true)) return *hit;
            }
        }

        // Pass 2, thorough: the requested pulse with the full searches (interleaver/FEC), then each
        // other pulse the same way until the time budget runs out.
        PipelineResult best = pipeline_finish_impl(prep, classification, base);
        if (best.decode_status != DecodeStatus::Verified) {
            for (const Variant& v : variants) {
                if (options.deadline && std::chrono::steady_clock::now() > *options.deadline) break;
                const bool same = v.shape == options.pulse_shape &&
                                  (v.shape == sigcore::PulseShape::Rect || v.rolloff == options.rolloff);
                if (same) continue;
                PipelineOptions alt = base;
                alt.pulse_shape = v.shape;
                alt.rolloff = v.rolloff;
                try {
                    PipelineResult cand = pipeline_finish_impl(prep, classification, alt);
                    if (cand.decode_status == DecodeStatus::Verified) {
                        cand.pulse_shape_searched = true;
                        return cand;
                    }
                } catch (const std::exception&) {
                    // a bad pulse model can fail outright; try the next
                }
            }
        }
        return best;
    }

    PipelineResult result;

    // Real-world signal support: a blind suggestion, from the raw capture alone,
    // before anything below assumes options.samples_per_symbol is actually right.
    // Best-effort: too short a capture is not a pipeline error, just no suggestion.
    try {
        result.symbol_rate_estimate = sigcore::estimate_symbol_rate(prep.signal.samples);
    } catch (const std::runtime_error&) {
        // capture too short for a usable estimate -- leave symbol_rate_estimate unset
    }

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
            const auto recovered = sigcore::recover_linear(prep.signal.samples, options.samples_per_symbol,
                                                            linear_scheme_from_name(scheme), options.pulse_shape, options.rolloff);
            processed = recovered.samples;
            result.recovery_used = true;
            result.pulse_shape_used = options.pulse_shape == sigcore::PulseShape::Rect
                                          ? std::string("rect")
                                          : "rrc " + printf_g(options.rolloff);
            result.recovery_phase_ambiguity = recovered.phase_ambiguity_count;
        } else {
            for (size_t i = 0; i < prep.signal.samples.size(); i += options.samples_per_symbol) {
                processed.push_back(prep.signal.samples[i]);
            }
        }
        samples_for_demod = &processed;
    }

    // Constellation check: the up-front one on raw 1-sample/symbol input, else on the recovered
    // symbol-rate samples (carrier offset already removed).
    if (!prep.constellation_label.empty()) {
        result.constellation_label = prep.constellation_label;
        result.constellation_c40 = prep.constellation_c40;
        result.constellation_c42 = prep.constellation_c42;
    } else if (scheme != "fsk" && result.recovery_used && samples_for_demod->size() >= 100) {
        const sigcore::ConstellationCheck cc = sigcore::constellation_check(*samples_for_demod);
        result.constellation_label = cc.best;
        result.constellation_c40 = cc.c40;
        result.constellation_c42 = cc.c42;
    }

    // Gap-list Phase 1.4: everything from demodulation through decode_status depends
    // only on `samples_for_demod` (plus scheme/options, both fixed for this run) --
    // pulled into its own function so it can be run more than once, below, on
    // different phase-rotated candidates of the same samples when recover_linear's
    // carrier loop left a rotational ambiguity (see RecoveryResult::phase_ambiguity_
    // count's own comment: a decision-directed PSK/QAM loop locks onto any of several
    // equally-valid constant phase offsets, with nothing in the signal itself to say
    // which one is upright). A single call, on the unrotated samples, is exactly the
    // original, single-candidate behaviour.
    const unsigned int ambiguity = result.recovery_phase_ambiguity.value_or(1);

    // Each analysis can run a full Parameter Search (seconds on a few thousand bits), and the wrong
    // rotations only ever fail after using their whole share of the time budget -- with four
    // rotations the right one can be left with none. A cheap probe (plain Viterbi at each bit offset,
    // no interleaver) says which rotation to try first; if that one verifies the others are skipped.
    std::optional<Analysis> preferred_result;
    if (result.recovery_used && ambiguity > 1 && options.auto_detect && !options.fec_scheme && !options.interleaver) {
        unsigned int preferred = 0;
        double best_score = rotation_probe_score(demodulate(*samples_for_demod, scheme, options.demod));
        const double unrotated_score = best_score;
        for (unsigned int k = 1; k < ambiguity; ++k) {
            const double angle = 2.0 * M_PI * static_cast<double>(k) / static_cast<double>(ambiguity);
            const std::complex<float> rot(static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle)));
            std::vector<std::complex<float>> rotated(samples_for_demod->size());
            for (size_t i = 0; i < rotated.size(); ++i) rotated[i] = (*samples_for_demod)[i] * rot;
            const double score = rotation_probe_score(demodulate(rotated, scheme, options.demod));
            if (score > best_score) {
                best_score = score;
                preferred = k;
            }
        }
        if (preferred != 0 && best_score >= 0.95 && unrotated_score < 0.95) {
            const double angle = 2.0 * M_PI * static_cast<double>(preferred) / static_cast<double>(ambiguity);
            const std::complex<float> rot(static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle)));
            std::vector<std::complex<float>> rotated(samples_for_demod->size());
            for (size_t i = 0; i < rotated.size(); ++i) rotated[i] = (*samples_for_demod)[i] * rot;
            Analysis first = analyze_samples(rotated, scheme, options, result.recovery_used);
            if (first.decode_status == DecodeStatus::Verified) preferred_result = std::move(first);
        }
    }

    const Analysis analysis = preferred_result ? *preferred_result : analyze_samples(*samples_for_demod, scheme, options, result.recovery_used);

    Analysis best = analysis;
    if (result.recovery_used && ambiguity > 1 && !preferred_result) {
        for (unsigned int k = 1; k < ambiguity; ++k) {
            const double angle = 2.0 * M_PI * static_cast<double>(k) / static_cast<double>(ambiguity);
            const std::complex<float> rot(static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle)));
            std::vector<std::complex<float>> rotated(samples_for_demod->size());
            for (size_t i = 0; i < rotated.size(); ++i) rotated[i] = (*samples_for_demod)[i] * rot;
            Analysis candidate = analyze_samples(rotated, scheme, options, result.recovery_used);
            // "Keep the one that verifies": once a rotation actually passes FEC
            // re-encode agreement, prefer it over the unrotated default. If none of
            // them verify, the unrotated candidate (`analysis`, tried first) stays --
            // no way to tell which rotation is right without a decode to check it
            // against, so this never makes an unverified result worse than before.
            if (candidate.decode_status == DecodeStatus::Verified && best.decode_status != DecodeStatus::Verified) {
                best = std::move(candidate);
            }
        }
    }

    result.demodulated_bits = std::move(best.demodulated_bits);
    result.rank_analysis = std::move(best.rank_analysis);
    result.detected_interleaver = std::move(best.detected_interleaver);
    result.detected_fec = std::move(best.detected_fec);
    result.detection_score = best.detection_score;
    result.deinterleaved_bits = std::move(best.deinterleaved_bits);
    result.decoded_bits = std::move(best.decoded_bits);
    result.soft_decoding_used = best.soft_decoding_used;
    result.reencode_agreement = best.reencode_agreement;
    result.raster = std::move(best.raster);
    result.header_pattern_source = best.header_pattern_source;
    result.boundaries = std::move(best.boundaries);
    result.decode_status = best.decode_status;
    result.param_search_used = best.param_search_used;
    result.param_search_bit_offset = best.param_search_bit_offset;
    result.param_search_confidence = best.param_search_confidence;
    result.auto_search_skipped = best.auto_search_skipped;
    result.frames = std::move(best.frames);
    result.framing_chain = std::move(best.framing_chain);

    result.file_path = prep.signal.path;
    result.sample_rate = prep.signal.sample_rate;
    result.num_samples = prep.signal.num_samples;
    result.file_format = prep.signal.format;
    result.samples = std::move(prep.signal.samples);
    {
        const Spectrogram& sp = prep.spectrogram;
        if (sp.frames > 0 && sp.nfft >= 8 && sp.data.size() >= static_cast<size_t>(sp.frames) * sp.nfft) {
            std::vector<double> mean(sp.nfft, 0.0);
            for (unsigned f = 0; f < sp.frames; ++f)
                for (unsigned k = 0; k < sp.nfft; ++k) mean[k] += sp.data[static_cast<size_t>(f) * sp.nfft + k];
            size_t peak = 0;
            for (unsigned k = 0; k < sp.nfft; ++k) {
                mean[k] /= sp.frames;
                if (mean[k] > mean[peak]) peak = k;
            }
            size_t lo = peak, hi = peak;
            while (lo > 0 && mean[lo - 1] >= mean[peak] - 10.0) --lo;
            while (hi + 1 < sp.nfft && mean[hi + 1] >= mean[peak] - 10.0) ++hi;
            const double bin_hz = prep.signal.sample_rate / sp.nfft;
            result.signal_offset_hz = sp.freqs[peak];
            result.signal_bandwidth_hz = static_cast<double>(hi - lo + 1) * bin_hz;
        }
    }
    result.spectrogram = std::move(prep.spectrogram);
    result.channel_candidates = std::move(prep.channel_candidates);
    result.channel_extracted = prep.channel_extracted;
    if (prep.channel_extracted && prep.channel_index && *prep.channel_index < result.channel_candidates.size()) {
        result.channel_used = result.channel_candidates[*prep.channel_index];
        result.channel_index = prep.channel_index;
    }
    result.channel_auto_selected = prep.channel_auto_selected;
    result.samples_per_symbol_used = options.samples_per_symbol;
    result.sample_rate_source = prep.sample_rate_source;
    result.center_freq_hz = prep.center_freq_hz;
    result.file_total_samples = prep.file_total_samples;
    result.window_start_samples = prep.window_start_samples;
    result.iq_format_guessed = prep.iq_format_guessed;
    result.iq_format_used = prep.iq_format_used;
    result.predicted_modulation = scheme;  // the classifier's label, or the override that replaced it
    result.predicted_confidence = classification.confidence;
    return result;
}

PipelineResult run_pipeline(const uint8_t* data, size_t size, const std::string& filename,
                            const PipelineOptions& options, const Classification& classification) {
    return pipeline_finish(pipeline_prepare(data, size, filename, options), classification, options);
}

}  // namespace sigapp
