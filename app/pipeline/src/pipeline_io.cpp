#include "pipeline_io.hpp"

#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace sigapp {

namespace {

constexpr uint32_t kMagic = 0x53494731;  // "SIG1"
// A length prefix above this is rejected before anything is allocated: a corrupt or hostile
// buffer must not be able to ask for gigabytes.
constexpr uint64_t kMaxElements = 1ull << 31;

class Writer {
public:
    std::vector<uint8_t> out;

    template <class T>
    void pod(T v) {
        static_assert(std::is_trivially_copyable_v<T>);
        const size_t at = out.size();
        out.resize(at + sizeof(T));
        std::memcpy(out.data() + at, &v, sizeof(T));
    }
    void b(bool v) { pod<uint8_t>(v ? 1 : 0); }
    void sz(size_t v) { pod<uint64_t>(v); }
    void str(const std::string& s) {
        sz(s.size());
        out.insert(out.end(), s.begin(), s.end());
    }
    template <class T>
    void vec(const std::vector<T>& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        sz(v.size());
        const size_t at = out.size();
        out.resize(at + v.size() * sizeof(T));
        if (!v.empty()) std::memcpy(out.data() + at, v.data(), v.size() * sizeof(T));
    }
    void bits(const Bits& v) {  // packed 8 per byte
        sz(v.size());
        const size_t at = out.size();
        out.resize(at + (v.size() + 7) / 8, 0);
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i]) out[at + i / 8] |= static_cast<uint8_t>(1u << (i % 8));
    }
    template <class T, class F>
    void opt(const std::optional<T>& v, F&& write) {
        b(v.has_value());
        if (v) write(*v);
    }
};

class Reader {
public:
    Reader(const uint8_t* d, size_t n) : d_(d), n_(n) {}

    template <class T>
    T pod() {
        need(sizeof(T));
        T v;
        std::memcpy(&v, d_ + pos_, sizeof(T));
        pos_ += sizeof(T);
        return v;
    }
    bool b() { return pod<uint8_t>() != 0; }
    size_t sz() {
        const uint64_t v = pod<uint64_t>();
        if (v > kMaxElements) throw std::runtime_error("pipeline_io: implausible length");
        return static_cast<size_t>(v);
    }
    std::string str() {
        const size_t n = sz();
        need(n);
        std::string s(reinterpret_cast<const char*>(d_ + pos_), n);
        pos_ += n;
        return s;
    }
    template <class T>
    std::vector<T> vec() {
        const size_t n = sz();
        if (n > (n_ - pos_) / sizeof(T)) throw std::runtime_error("pipeline_io: truncated");
        std::vector<T> v(n);
        if (n) std::memcpy(v.data(), d_ + pos_, n * sizeof(T));
        pos_ += n * sizeof(T);
        return v;
    }
    Bits bits() {
        const size_t n = sz();
        need((n + 7) / 8);
        Bits v(n);
        for (size_t i = 0; i < n; ++i) v[i] = (d_[pos_ + i / 8] >> (i % 8)) & 1u;
        pos_ += (n + 7) / 8;
        return v;
    }
    template <class T, class F>
    std::optional<T> opt(F&& read) {
        if (!b()) return std::nullopt;
        return read();
    }
    void finish() const {
        if (pos_ != n_) throw std::runtime_error("pipeline_io: trailing bytes");
    }

private:
    void need(size_t n) const {
        if (n > n_ - pos_) throw std::runtime_error("pipeline_io: truncated");
    }
    const uint8_t* d_;
    size_t n_;
    size_t pos_ = 0;
};

// ---- shared small structs --------------------------------------------------------------

void write_interleaver(Writer& w, const InterleaverSpec& s) {
    w.str(s.kind);
    for (const auto* f : {&s.rows, &s.cols, &s.seed, &s.branches, &s.delay_increment})
        w.opt(*f, [&](uint32_t v) { w.pod(v); });
}
InterleaverSpec read_interleaver(Reader& r) {
    InterleaverSpec s;
    s.kind = r.str();
    for (auto* f : {&s.rows, &s.cols, &s.seed, &s.branches, &s.delay_increment})
        *f = r.opt<uint32_t>([&] { return r.pod<uint32_t>(); });
    return s;
}

void write_channel(Writer& w, const sigcore::ChannelCandidate& c) {
    w.pod(c.center_freq_hz);
    w.pod(c.bandwidth_hz);
    w.pod(c.confidence);
}
sigcore::ChannelCandidate read_channel(Reader& r) {
    sigcore::ChannelCandidate c;
    c.center_freq_hz = r.pod<double>();
    c.bandwidth_hz = r.pod<double>();
    c.confidence = r.pod<double>();
    return c;
}

template <class E>
E read_enum(Reader& r, uint8_t max_value) {
    const uint8_t v = r.pod<uint8_t>();
    if (v > max_value) throw std::runtime_error("pipeline_io: enum value out of range");
    return static_cast<E>(v);
}

}  // namespace

// ---- options -----------------------------------------------------------------------------

std::vector<uint8_t> serialize_options(const PipelineOptions& o) {
    Writer w;
    w.pod(kMagic);
    w.opt(o.sample_rate, [&](double v) { w.pod(v); });
    w.pod<uint8_t>(static_cast<uint8_t>(o.iq_format));
    w.b(o.iq_format_auto);
    w.opt(o.sigmf_meta_json, [&](const std::string& v) { w.str(v); });
    w.pod(o.samples_per_symbol);
    w.opt(o.samples_per_symbol_exact, [&](double v) { w.pod(v); });
    w.sz(o.window_start_samples);
    w.sz(o.window_samples);
    w.sz(o.source_total_bytes);
    w.sz(o.source_start_byte);
    w.pod(o.demod.bits_per_symbol);
    w.pod(o.demod.samples_per_symbol);
    w.pod(o.demod.bandwidth);
    w.b(o.fsk_auto);
    w.opt(o.interleaver, [&](const InterleaverSpec& v) { write_interleaver(w, v); });
    w.opt(o.fec_scheme, [&](const std::string& v) { w.str(v); });
    w.opt(o.fec_msg_len, [&](size_t v) { w.sz(v); });
    w.opt(o.header_pattern, [&](const Bits& v) { w.bits(v); });
    w.b(o.auto_detect);
    w.b(o.soft_decoding);
    w.b(o.recover_timing);
    w.pod<uint8_t>(static_cast<uint8_t>(o.pulse_shape));
    w.pod(o.rolloff);
    w.b(o.pulse_search_done);
    w.pod(o.time_budget_sec);
    w.b(o.find_channels_enabled);
    w.opt(o.select_channel, [&](size_t v) { w.sz(v); });
    w.b(o.auto_extract_channel);
    w.opt(o.modulation_override, [&](const std::string& v) { w.str(v); });
    return std::move(w.out);
}

PipelineOptions deserialize_options(const uint8_t* data, size_t size) {
    Reader r(data, size);
    if (r.pod<uint32_t>() != kMagic) throw std::runtime_error("pipeline_io: not a serialized PipelineOptions");
    PipelineOptions o;
    o.sample_rate = r.opt<double>([&] { return r.pod<double>(); });
    o.iq_format = read_enum<sigcore::IqSampleFormat>(r, 2);
    o.iq_format_auto = r.b();
    o.sigmf_meta_json = r.opt<std::string>([&] { return r.str(); });
    o.samples_per_symbol = r.pod<unsigned int>();
    o.samples_per_symbol_exact = r.opt<double>([&] { return r.pod<double>(); });
    o.window_start_samples = r.sz();
    o.window_samples = r.sz();
    o.source_total_bytes = r.sz();
    o.source_start_byte = r.sz();
    o.demod.bits_per_symbol = r.pod<unsigned int>();
    o.demod.samples_per_symbol = r.pod<unsigned int>();
    o.demod.bandwidth = r.pod<float>();
    o.fsk_auto = r.b();
    o.interleaver = r.opt<InterleaverSpec>([&] { return read_interleaver(r); });
    o.fec_scheme = r.opt<std::string>([&] { return r.str(); });
    o.fec_msg_len = r.opt<size_t>([&] { return r.sz(); });
    o.header_pattern = r.opt<Bits>([&] { return r.bits(); });
    o.auto_detect = r.b();
    o.soft_decoding = r.b();
    o.recover_timing = r.b();
    o.pulse_shape = read_enum<sigcore::PulseShape>(r, 1);
    o.rolloff = r.pod<float>();
    o.pulse_search_done = r.b();
    o.time_budget_sec = r.pod<double>();
    o.find_channels_enabled = r.b();
    o.select_channel = r.opt<size_t>([&] { return r.sz(); });
    o.auto_extract_channel = r.b();
    o.modulation_override = r.opt<std::string>([&] { return r.str(); });
    r.finish();
    return o;
}

// ---- classification ------------------------------------------------------------------------

namespace {
void write_class(Writer& w, const Classification& c, int depth) {
    w.str(c.label);
    w.pod(c.confidence);
    w.sz(c.alternatives.size());
    if (depth > 4) {
        if (!c.alternatives.empty()) throw std::runtime_error("pipeline_io: classification nested too deeply");
        return;
    }
    for (const auto& a : c.alternatives) write_class(w, a, depth + 1);
}
Classification read_class(Reader& r, int depth) {
    Classification c;
    c.label = r.str();
    c.confidence = r.pod<double>();
    const size_t n = r.sz();
    if (depth > 4 && n != 0) throw std::runtime_error("pipeline_io: classification nested too deeply");
    if (n > 64) throw std::runtime_error("pipeline_io: implausible number of alternatives");
    for (size_t i = 0; i < n; ++i) c.alternatives.push_back(read_class(r, depth + 1));
    return c;
}
}  // namespace

std::vector<uint8_t> serialize_classification(const Classification& c) {
    Writer w;
    w.pod(kMagic);
    write_class(w, c, 0);
    return std::move(w.out);
}

Classification deserialize_classification(const uint8_t* data, size_t size) {
    Reader r(data, size);
    if (r.pod<uint32_t>() != kMagic) throw std::runtime_error("pipeline_io: not a serialized Classification");
    Classification c = read_class(r, 0);
    r.finish();
    return c;
}

// ---- result --------------------------------------------------------------------------------

std::vector<uint8_t> serialize_result(const PipelineResult& p) {
    Writer w;
    w.pod(kMagic);
    w.str(p.file_path);
    w.pod(p.sample_rate);
    w.sz(p.num_samples);
    w.str(p.file_format);
    w.vec(p.samples);
    w.vec(p.spectrogram.data);
    w.pod(p.spectrogram.frames);
    w.pod(p.spectrogram.nfft);
    w.vec(p.spectrogram.freqs);
    w.vec(p.spectrogram.times);
    w.str(p.predicted_modulation);
    w.pod(p.predicted_confidence);
    w.bits(p.demodulated_bits);
    w.opt(p.deinterleaved_bits, [&](const Bits& v) { w.bits(v); });
    w.opt(p.decoded_bits, [&](const Bits& v) { w.bits(v); });
    // boundaries
    w.bits(p.boundaries.header_pattern);
    w.opt(p.boundaries.frame_period, [&](int v) { w.pod(v); });
    w.vec(p.boundaries.header_positions);
    w.sz(p.boundaries.payloads.size());
    for (const auto& pl : p.boundaries.payloads) w.bits(pl);
    w.pod<uint8_t>(static_cast<uint8_t>(p.header_pattern_source));
    w.opt(p.detected_interleaver, [&](const InterleaverSpec& v) { write_interleaver(w, v); });
    w.opt(p.detected_fec, [&](const std::string& v) { w.str(v); });
    w.opt(p.detection_score, [&](int v) { w.pod(v); });
    // raster
    w.b(p.raster.detected);
    w.pod(p.raster.frame_length);
    w.pod(p.raster.num_frames);
    w.bits(p.raster.matrix);
    w.vec(p.raster.row_shift);
    w.sz(p.raster.columns.size());
    for (const auto& c : p.raster.columns) {
        w.pod(c.ones_fraction);
        w.pod(c.stability);
        w.pod(c.transition_rate);
        w.pod<uint8_t>(static_cast<uint8_t>(c.label));
    }
    w.bits(p.raster.sync_word);
    w.vec(p.raster.header_columns);
    w.vec(p.raster.sync_positions);
    // rank analysis
    w.b(p.rank_analysis.detected);
    w.pod(p.rank_analysis.width);
    w.pod(p.rank_analysis.rank);
    w.pod(p.rank_analysis.code_rate);
    w.pod(p.rank_analysis.windows_used);
    w.sz(p.rank_analysis.profile.size());
    for (const auto& pt : p.rank_analysis.profile) {
        w.pod(pt.width);
        w.pod(pt.rank);
        w.pod(pt.baseline_deficiency);
        w.pod(pt.excess_deficiency);
    }
    w.b(p.soft_decoding_used);
    w.opt(p.reencode_agreement, [&](double v) { w.pod(v); });
    w.b(p.recovery_used);
    w.opt(p.recovery_phase_ambiguity, [&](unsigned v) { w.pod(v); });
    w.opt(p.symbol_rate_estimate, [&](const sigcore::SymbolRateEstimate& v) {
        w.pod(v.samples_per_symbol);
        w.pod(v.confidence);
    });
    w.pod(p.samples_per_symbol_used);
    w.opt(p.samples_per_symbol_fractional, [&](double v) { w.pod(v); });
    w.sz(p.channel_candidates.size());
    for (const auto& c : p.channel_candidates) write_channel(w, c);
    w.b(p.channel_extracted);
    w.opt(p.channel_used, [&](const sigcore::ChannelCandidate& v) { write_channel(w, v); });
    w.b(p.channel_auto_selected);
    w.opt(p.channel_index, [&](size_t v) { w.sz(v); });
    w.pod<uint8_t>(static_cast<uint8_t>(p.sample_rate_source));
    w.sz(p.file_total_samples);
    w.sz(p.window_start_samples);
    w.opt(p.center_freq_hz, [&](double v) { w.pod(v); });
    w.b(p.iq_format_guessed);
    w.str(p.iq_format_used);
    w.opt(p.signal_offset_hz, [&](double v) { w.pod(v); });
    w.opt(p.signal_bandwidth_hz, [&](double v) { w.pod(v); });
    w.pod<uint8_t>(static_cast<uint8_t>(p.decode_status));
    w.b(p.param_search_used);
    w.pod(p.param_search_bit_offset);
    w.pod(p.param_search_confidence);
    w.b(p.auto_search_skipped);
    w.b(p.modulation_from_search);
    w.str(p.classifier_top_label);
    w.b(p.pulse_shape_searched);
    w.b(p.fsk_params_estimated);
    w.pod(p.fsk_bits_per_symbol);
    w.pod(p.fsk_samples_per_symbol);
    w.pod(p.fsk_bandwidth);
    w.pod(p.fsk_confidence);
    w.str(p.pulse_shape_used);
    w.sz(p.frames.size());
    for (const auto& f : p.frames) {
        w.str(f.destination);
        w.str(f.source);
        w.sz(f.digipeaters.size());
        for (const auto& d : f.digipeaters) w.str(d);
        w.pod(f.control);
        w.b(f.has_pid);
        w.pod(f.pid);
        w.vec(f.info);
        w.vec(f.raw);
        w.sz(f.bit_offset);
    }
    w.str(p.framing_chain);
    w.b(p.modulation_from_blind_check);
    w.pod(p.blind_line);
    w.pod(p.blind_envelope_cv);
    w.str(p.constellation_label);
    w.pod(p.constellation_c40);
    w.pod(p.constellation_c42);
    w.b(p.modulation_from_constellation);
    return std::move(w.out);
}

PipelineResult deserialize_result(const uint8_t* data, size_t size) {
    Reader r(data, size);
    if (r.pod<uint32_t>() != kMagic) throw std::runtime_error("pipeline_io: not a serialized PipelineResult");
    PipelineResult p;
    p.file_path = r.str();
    p.sample_rate = r.pod<double>();
    p.num_samples = r.sz();
    p.file_format = r.str();
    p.samples = r.vec<std::complex<float>>();
    p.spectrogram.data = r.vec<float>();
    p.spectrogram.frames = r.pod<unsigned int>();
    p.spectrogram.nfft = r.pod<unsigned int>();
    p.spectrogram.freqs = r.vec<double>();
    p.spectrogram.times = r.vec<double>();
    p.predicted_modulation = r.str();
    p.predicted_confidence = r.pod<double>();
    p.demodulated_bits = r.bits();
    p.deinterleaved_bits = r.opt<Bits>([&] { return r.bits(); });
    p.decoded_bits = r.opt<Bits>([&] { return r.bits(); });
    p.boundaries.header_pattern = r.bits();
    p.boundaries.frame_period = r.opt<int>([&] { return r.pod<int>(); });
    p.boundaries.header_positions = r.vec<size_t>();
    const size_t payloads = r.sz();
    for (size_t i = 0; i < payloads; ++i) p.boundaries.payloads.push_back(r.bits());
    p.header_pattern_source = read_enum<HeaderPatternSource>(r, 2);
    p.detected_interleaver = r.opt<InterleaverSpec>([&] { return read_interleaver(r); });
    p.detected_fec = r.opt<std::string>([&] { return r.str(); });
    p.detection_score = r.opt<int>([&] { return r.pod<int>(); });
    p.raster.detected = r.b();
    p.raster.frame_length = r.pod<unsigned>();
    p.raster.num_frames = r.pod<unsigned>();
    p.raster.matrix = r.bits();
    p.raster.row_shift = r.vec<int>();
    const size_t cols = r.sz();
    for (size_t i = 0; i < cols; ++i) {
        RasterColumn c;
        c.ones_fraction = r.pod<double>();
        c.stability = r.pod<double>();
        c.transition_rate = r.pod<double>();
        c.label = read_enum<ColumnLabel>(r, 2);
        p.raster.columns.push_back(c);
    }
    p.raster.sync_word = r.bits();
    p.raster.header_columns = r.vec<size_t>();
    p.raster.sync_positions = r.vec<size_t>();
    p.rank_analysis.detected = r.b();
    p.rank_analysis.width = r.pod<unsigned>();
    p.rank_analysis.rank = r.pod<double>();
    p.rank_analysis.code_rate = r.pod<double>();
    p.rank_analysis.windows_used = r.pod<unsigned>();
    const size_t profile = r.sz();
    for (size_t i = 0; i < profile; ++i) {
        RankProfilePoint pt;
        pt.width = r.pod<unsigned>();
        pt.rank = r.pod<double>();
        pt.baseline_deficiency = r.pod<double>();
        pt.excess_deficiency = r.pod<double>();
        p.rank_analysis.profile.push_back(pt);
    }
    p.soft_decoding_used = r.b();
    p.reencode_agreement = r.opt<double>([&] { return r.pod<double>(); });
    p.recovery_used = r.b();
    p.recovery_phase_ambiguity = r.opt<unsigned int>([&] { return r.pod<unsigned int>(); });
    p.symbol_rate_estimate = r.opt<sigcore::SymbolRateEstimate>([&] {
        sigcore::SymbolRateEstimate e;
        e.samples_per_symbol = r.pod<double>();
        e.confidence = r.pod<double>();
        return e;
    });
    p.samples_per_symbol_used = r.pod<unsigned int>();
    p.samples_per_symbol_fractional = r.opt<double>([&] { return r.pod<double>(); });
    const size_t chans = r.sz();
    for (size_t i = 0; i < chans; ++i) p.channel_candidates.push_back(read_channel(r));
    p.channel_extracted = r.b();
    p.channel_used = r.opt<sigcore::ChannelCandidate>([&] { return read_channel(r); });
    p.channel_auto_selected = r.b();
    p.channel_index = r.opt<size_t>([&] { return r.sz(); });
    p.sample_rate_source = read_enum<SampleRateSource>(r, 3);
    p.file_total_samples = r.sz();
    p.window_start_samples = r.sz();
    p.center_freq_hz = r.opt<double>([&] { return r.pod<double>(); });
    p.iq_format_guessed = r.b();
    p.iq_format_used = r.str();
    p.signal_offset_hz = r.opt<double>([&] { return r.pod<double>(); });
    p.signal_bandwidth_hz = r.opt<double>([&] { return r.pod<double>(); });
    p.decode_status = read_enum<DecodeStatus>(r, 2);
    p.param_search_used = r.b();
    p.param_search_bit_offset = r.pod<unsigned int>();
    p.param_search_confidence = r.pod<double>();
    p.auto_search_skipped = r.b();
    p.modulation_from_search = r.b();
    p.classifier_top_label = r.str();
    p.pulse_shape_searched = r.b();
    p.fsk_params_estimated = r.b();
    p.fsk_bits_per_symbol = r.pod<unsigned int>();
    p.fsk_samples_per_symbol = r.pod<unsigned int>();
    p.fsk_bandwidth = r.pod<float>();
    p.fsk_confidence = r.pod<double>();
    p.pulse_shape_used = r.str();
    const size_t nframes = r.sz();
    for (size_t i = 0; i < nframes; ++i) {
        Ax25Frame f;
        f.destination = r.str();
        f.source = r.str();
        const size_t ndig = r.sz();
        for (size_t k = 0; k < ndig; ++k) f.digipeaters.push_back(r.str());
        f.control = r.pod<uint8_t>();
        f.has_pid = r.b();
        f.pid = r.pod<uint8_t>();
        f.info = r.vec<uint8_t>();
        f.raw = r.vec<uint8_t>();
        f.bit_offset = r.sz();
        p.frames.push_back(std::move(f));
    }
    p.framing_chain = r.str();
    p.modulation_from_blind_check = r.b();
    p.blind_line = r.pod<double>();
    p.blind_envelope_cv = r.pod<double>();
    p.constellation_label = r.str();
    p.constellation_c40 = r.pod<double>();
    p.constellation_c42 = r.pod<double>();
    p.modulation_from_constellation = r.b();
    r.finish();
    return p;
}

}  // namespace sigapp
