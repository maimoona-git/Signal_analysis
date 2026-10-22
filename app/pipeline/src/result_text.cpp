#include "result_text.hpp"

#include <cstdint>
#include <cstdio>

#include "correlate.hpp"

namespace sigapp {

namespace {

std::string printf_string(const char* format, double value) {
    char buf[64];
    std::snprintf(buf, sizeof buf, format, value);
    return buf;
}

// Python prints an absent optional as None.
std::string or_none(const std::optional<std::string>& v) { return v ? *v : "None"; }

std::string hex_byte(uint8_t v) {
    static const char* d = "0123456789abcdef";
    return std::string(1, d[v >> 4]) + d[v & 15];
}

std::string bits_string(const Bits& bits) {
    std::string s;
    for (uint8_t b : bits) s += b ? '1' : '0';
    return s;
}

}  // namespace

std::string format_g(double value) { return printf_string("%g", value); }

std::string python_repr(const std::string& text) {
    const bool has_single = text.find('\'') != std::string::npos;
    const bool has_double = text.find('"') != std::string::npos;
    const char quote = (has_single && !has_double) ? '"' : '\'';

    std::string out(1, quote);
    for (unsigned char c : text) {
        if (c == static_cast<unsigned char>(quote) || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c < 32 || c >= 127) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02x", c);
            out += buf;
        } else {
            out += static_cast<char>(c);
        }
    }
    out += quote;
    return out;
}

std::string format_result_text(const PipelineResult& r, size_t max_message_chars, bool show_soft_decoding_info,
                               bool show_decode_status) {
    const FrameBoundaries& b = r.boundaries;
    std::string t;
    auto line = [&](const std::string& s) { t += s + "\n"; };

    line("File:                 " + r.file_path);
    line("Format:               " + r.file_format);
    line("Sample rate:          " + format_g(r.sample_rate) + " Hz");
    // Gap-list Phase 1.6: same opt-in as the rest of this function's "full parameter
    // display" additions -- see show_decode_status's own doc comment.
    if (show_decode_status) {
        switch (r.sample_rate_source) {
            case SampleRateSource::FromFileHeader: line("Sample rate source:   from file header"); break;
            case SampleRateSource::UserSpecified: line("Sample rate source:   user-specified"); break;
            case SampleRateSource::AssumedDefault: line("Sample rate source:   assumed default (1 MHz)"); break;
            case SampleRateSource::FromSigmfMeta: line("Sample rate source:   SigMF metadata"); break;
        }
        if (r.file_total_samples > 0 && r.num_samples < r.file_total_samples) {
            line("Analysed window:      samples " + std::to_string(r.window_start_samples) + " to " +
                 std::to_string(r.window_start_samples + r.num_samples) + " of " +
                 std::to_string(r.file_total_samples) + " (the rest of the file was not analysed)");
        }
        if (!r.iq_format_used.empty()) {
            line("IQ sample format:     " + r.iq_format_used + (r.iq_format_guessed ? " (guessed from the data)" : ""));
        }
        if (r.signal_offset_hz && r.signal_bandwidth_hz) {
            line("Strongest signal:     " + format_g(*r.signal_offset_hz) + " Hz from center, ~" +
                 format_g(*r.signal_bandwidth_hz) + " Hz wide (within 10 dB of the peak)");
        }
        if (!r.frames.empty()) {
            const Ax25Frame& f = r.frames.front();
            std::string via;
            for (const auto& d : f.digipeaters) via += "," + d;
            line("AX.25 frames:         " + std::to_string(r.frames.size()) + " found, CRC-16 valid (" + r.framing_chain + ")");
            line("  first frame:        " + f.source + " > " + f.destination + via + ", control 0x" +
                 hex_byte(f.control) +
                 (f.has_pid ? ", PID 0x" + hex_byte(f.pid) : std::string()) + ", " + std::to_string(f.info.size()) +
                 " info bytes");
        }
        if (r.auto_window_total_samples > 0 || r.auto_window_total_bits > 0) {
            std::string what;
            if (r.auto_window_total_samples > 0)
                what = "the first " + std::to_string(r.samples.size()) + " of " + std::to_string(r.auto_window_total_samples) + " samples";
            if (r.auto_window_total_bits > 0)
                what += std::string(what.empty() ? "" : " and ") + "the first " + std::to_string(r.demodulated_bits.size()) + " of " +
                        std::to_string(r.auto_window_total_bits) + " demodulated bits";
            line("Auto-detect window:   searched " + what + " (capture too large to search whole); this result covers only that part");
        }
        if (r.param_search_used) {
            line("Parameter search:     found automatically at bit offset " + std::to_string(r.param_search_bit_offset) +
                 " (confidence " + printf_string("%.2f", r.param_search_confidence) + ")");
        }
        if (r.fsk_params_estimated) {
            line("FSK parameters:       " + std::to_string(1u << r.fsk_bits_per_symbol) + " tones, " +
                 std::to_string(r.fsk_samples_per_symbol) + " samples/symbol, bandwidth " +
                 printf_string("%.3f", r.fsk_bandwidth) + " (estimated, confidence " +
                 printf_string("%.2f", r.fsk_confidence) + ")");
        }
        if (r.recovery_used && !r.pulse_shape_used.empty()) {
            line("Pulse shape:          " + r.pulse_shape_used + (r.pulse_shape_searched ? " (found by search)" : ""));
        }
        if (r.center_freq_hz) line("Center frequency:     " + format_g(*r.center_freq_hz) + " Hz (SigMF metadata)");
    }
    line("Num samples:          " + std::to_string(r.num_samples));
    line("Spectrogram shape:    (" + std::to_string(r.spectrogram.frames) + ", " + std::to_string(r.spectrogram.nfft) + ")");
    line("");
    // A confidence is the classifier's; when the constellation check supplied the label there is none to show.
    line("Predicted modulation: " + r.predicted_modulation +
         (r.modulation_from_constellation ? std::string(" (read from the constellation)")
                                          : " (confidence " + printf_string("%.2f", r.predicted_confidence) + ")"));
    if (show_decode_status && r.modulation_from_blind_check) {
        line("Modulation check:     classifier said " + r.classifier_top_label + ", but the signal is not FSK (envelope varies " +
             printf_string("%.2f", r.blind_envelope_cv) + ", squared-signal line " + printf_string("%.2f", r.blind_line) +
             "); analysed as " + r.predicted_modulation);
    }
    if (show_decode_status && !r.constellation_label.empty()) {
        line("Constellation check:  reads as " + r.constellation_label + " (|C40| " + printf_string("%.2f", r.constellation_c40) +
             ", C42 " + printf_string("%.2f", r.constellation_c42) + ")" +
             (r.modulation_from_constellation ? "; this is the reading that verified" : ""));
    }
    if (show_decode_status && r.modulation_from_search) {
        line("Modulation search:    classifier's top guess was " + r.classifier_top_label +
             "; " + r.predicted_modulation + " is the one that verified");
    }
    line("Demodulated bits:     " + std::to_string(r.demodulated_bits.size()));
    if (show_decode_status) {
        line("Samples/symbol used:  " + std::to_string(r.samples_per_symbol_used));
        if (r.symbol_rate_estimate) {
            line("Symbol-rate estimate: " + printf_string("%.1f", r.symbol_rate_estimate->samples_per_symbol) +
                 " samples/symbol (confidence " + printf_string("%.2f", r.symbol_rate_estimate->confidence) + ")");
        }
        if (r.channel_used) {
            line("Channel used:         center " + format_g(r.channel_used->center_freq_hz) + " Hz, bandwidth " +
                 format_g(r.channel_used->bandwidth_hz) + " Hz (confidence " +
                 printf_string("%.2f", r.channel_used->confidence) + ")" +
                 (r.channel_auto_selected ? ", chosen automatically" : ""));
        }
    }
    if (r.detection_score.has_value()) {
        line("Detected interleaver: " +
             or_none(r.detected_interleaver ? std::optional<std::string>(r.detected_interleaver->kind) : std::nullopt) +
             " (score " + std::to_string(*r.detection_score) + ")");
        line("Detected FEC:         " + or_none(r.detected_fec));
    }
    if (r.deinterleaved_bits.has_value()) line("Deinterleaved bits:   " + std::to_string(r.deinterleaved_bits->size()));
    if (r.decoded_bits.has_value()) line("FEC-decoded bits:     " + std::to_string(r.decoded_bits->size()));
    if (show_soft_decoding_info) {
        if (r.decoded_bits.has_value()) {
            line(std::string("Decoding mode:        ") + (r.soft_decoding_used ? "soft (LLR)" : "hard"));
        }
        if (r.reencode_agreement.has_value()) {
            line("Re-encode agreement:  " + printf_string("%.3f", *r.reencode_agreement));
        }
    }

    const Bits& final_bits = r.decoded_bits ? *r.decoded_bits : r.deinterleaved_bits ? *r.deinterleaved_bits : r.demodulated_bits;
    std::string message = bits_to_text(final_bits);
    // Gap-list Phase 1.1: see format_result_text's own doc comment on show_decode_status
    // for why the label (and, for Unverified, whether this is even called "decoded" at
    // all) changes here instead of always saying "Decoded message" unconditionally.
    std::string label = "Decoded message:      ";
    if (show_decode_status) {
        switch (r.decode_status) {
            case DecodeStatus::Verified: label = "Decoded message (verified):      "; break;
            case DecodeStatus::Structured: label = "Decoded message (structured, not FEC-verified):      "; break;
            case DecodeStatus::Unverified: label = "Raw bits (unverified):      "; break;
        }
    }
    if (show_decode_status && r.decode_status == DecodeStatus::Unverified) {
        line("No verified decode.");
    }
    if (message.size() > max_message_chars) {
        const size_t total = message.size();
        message.resize(max_message_chars);
        line(label + python_repr(message) + " ... (" + std::to_string(total) +
             " characters in all, the first " + std::to_string(max_message_chars) + " shown)");
    } else {
        line(label + python_repr(message));
    }

    // Gap-list Phase 1.6: the rank analysis (Feature 2) and frame raster (Feature 4)
    // already have their own dedicated panels (format_rank_analysis_text,
    // format_raster_text) -- these lines are a SUMMARY alongside everything else
    // above, for a caller (or a saved export) that only looks at this one panel.
    if (show_decode_status) {
        if (r.rank_analysis.detected) {
            line("Code length (rank):   " + std::to_string(r.rank_analysis.width) + " bits, rate " +
                 printf_string("%.3f", r.rank_analysis.code_rate));
        }
        if (r.raster.detected) {
            line("Frame length:         " + std::to_string(r.raster.frame_length) + " bits");
            line("Sync word (raster):   " + bits_string(r.raster.sync_word));
        }
    }

    line("");
    std::string header_pattern_line = "Header pattern:       " + bits_string(b.header_pattern);
    // Gap-list Phase 1.3: same opt-in as show_decode_status -- both are about not
    // presenting a guess as evidence. See HeaderPatternSource's own comment.
    if (show_decode_status) {
        switch (r.header_pattern_source) {
            case HeaderPatternSource::UserSpecified: header_pattern_line += " (user-specified)"; break;
            case HeaderPatternSource::FrameRaster: header_pattern_line += " (from frame raster)"; break;
            case HeaderPatternSource::Assumed: header_pattern_line += " (assumed: first 16 bits)"; break;
        }
    }
    line(header_pattern_line);
    line("Frame period:         " + (b.frame_period ? std::to_string(*b.frame_period) : std::string("None")));
    std::string positions = "[";
    for (size_t i = 0; i < b.header_positions.size() && i < 10; ++i) {
        positions += (i ? ", " : "") + std::to_string(b.header_positions[i]);
    }
    positions += "]";
    if (b.header_positions.size() > 10) positions += " ...";
    line("Header positions:     " + positions);
    t += "Payload spans found:  " + std::to_string(b.payloads.size());  // the original has no trailing newline
    return t;
}

std::string format_done_status(const PipelineResult& r, bool show_decode_status) {
    std::string status = "Done. Predicted modulation: " + r.predicted_modulation +
                         (r.modulation_from_constellation ? std::string(" (read from the constellation).")
                                                          : " (" + printf_string("%.0f", r.predicted_confidence * 100.0) + "% confidence).");
    if (show_decode_status) {
        switch (r.decode_status) {
            case DecodeStatus::Verified: status += " Decoded message verified."; break;
            case DecodeStatus::Structured: status += " Decode is structured (not FEC-verified)."; break;
            case DecodeStatus::Unverified: status += " No verified decode."; break;
        }
    }
    return status;
}

namespace {

// MSB-first into bytes, zero-padded to a whole byte (the same convention as fec_dispatch's
// bits_to_bytes), then hex-encoded -- purely a display convenience for a bit pattern that
// need not be byte-aligned.
std::string bits_to_hex(const Bits& bits) {
    static const char* digits = "0123456789abcdef";
    std::string hex;
    for (size_t i = 0; i < bits.size(); i += 8) {
        unsigned byte = 0;
        for (int b = 0; b < 8; ++b) {
            const size_t idx = i + static_cast<size_t>(b);
            byte = (byte << 1) | (idx < bits.size() && bits[idx] ? 1u : 0u);
        }
        hex += digits[byte >> 4];
        hex += digits[byte & 15];
    }
    return hex.empty() ? "-" : hex;
}

}  // namespace

namespace {

std::string json_escape(const std::string& in) {
    std::string out;
    for (unsigned char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 32 || c >= 127) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string jstr(const std::string& v) { return "\"" + json_escape(v) + "\""; }
std::string jnum(double v) { return printf_string("%.17g", v); }

}  // namespace

std::string format_result_json(const PipelineResult& r) {
    const char* status = r.decode_status == DecodeStatus::Verified ? "verified"
                         : r.decode_status == DecodeStatus::Structured ? "structured" : "unverified";
    const char* rate_src = r.sample_rate_source == SampleRateSource::FromFileHeader ? "file_header"
                           : r.sample_rate_source == SampleRateSource::UserSpecified ? "user"
                           : r.sample_rate_source == SampleRateSource::FromSigmfMeta ? "sigmf_meta" : "assumed_default";
    const char* hdr_src = r.header_pattern_source == HeaderPatternSource::UserSpecified ? "user"
                          : r.header_pattern_source == HeaderPatternSource::FrameRaster ? "frame_raster" : "assumed";
    const Bits& final_bits = r.decoded_bits ? *r.decoded_bits : r.deinterleaved_bits ? *r.deinterleaved_bits : r.demodulated_bits;
    std::string j = "{\n";
    auto field = [&](const std::string& k, const std::string& v, bool last = false) {
        j += "  " + jstr(k) + ": " + v + (last ? "\n" : ",\n");
    };
    field("file", jstr(r.file_path));
    field("format", jstr(r.file_format));
    field("sample_rate_hz", jnum(r.sample_rate));
    field("sample_rate_source", jstr(rate_src));
    if (r.center_freq_hz) field("center_frequency_hz", jnum(*r.center_freq_hz));
    if (r.signal_offset_hz) field("strongest_signal_offset_hz", jnum(*r.signal_offset_hz));
    if (r.signal_bandwidth_hz) field("strongest_signal_bandwidth_hz", jnum(*r.signal_bandwidth_hz));
    field("num_samples", std::to_string(r.num_samples));
    field("modulation", jstr(r.predicted_modulation));
    field("modulation_confidence", jnum(r.predicted_confidence));
    if (r.modulation_from_search || r.modulation_from_blind_check) field("classifier_top_label", jstr(r.classifier_top_label));
    if (!r.constellation_label.empty()) {
        field("constellation_label", jstr(r.constellation_label));
        field("constellation_c40", jnum(r.constellation_c40));
        field("constellation_c42", jnum(r.constellation_c42));
        if (r.modulation_from_constellation) field("modulation_from_constellation", "true");
    }
    if (r.modulation_from_blind_check) {
        field("modulation_from_blind_check", "true");
        field("blind_envelope_cv", jnum(r.blind_envelope_cv));
        field("blind_line", jnum(r.blind_line));
    }
    field("samples_per_symbol_used", std::to_string(r.samples_per_symbol_used));
    if (r.symbol_rate_estimate) {
        field("symbol_rate_estimate_samples_per_symbol", jnum(r.symbol_rate_estimate->samples_per_symbol));
        field("symbol_rate_estimate_confidence", jnum(r.symbol_rate_estimate->confidence));
    }
    if (r.channel_used) {
        field("channel_center_hz", jnum(r.channel_used->center_freq_hz));
        field("channel_bandwidth_hz", jnum(r.channel_used->bandwidth_hz));
    }
    field("recovery_used", r.recovery_used ? "true" : "false");
    field("demodulated_bits", std::to_string(r.demodulated_bits.size()));
    if (r.detected_interleaver) field("interleaver", jstr(r.detected_interleaver->kind));
    if (r.detected_fec) field("fec", jstr(*r.detected_fec));
    field("auto_window_total_samples", std::to_string(r.auto_window_total_samples));
    field("auto_window_total_bits", std::to_string(r.auto_window_total_bits));
    if (!r.frames.empty()) {
        field("framing_chain", jstr(r.framing_chain));
        std::string arr = "[";
        for (size_t i = 0; i < r.frames.size(); ++i) {
            const Ax25Frame& f = r.frames[i];
            std::string digis = "[";
            for (size_t k = 0; k < f.digipeaters.size(); ++k) digis += (k ? ", " : "") + jstr(f.digipeaters[k]);
            digis += "]";
            std::string info_hex;
            for (uint8_t b : f.info) info_hex += hex_byte(b);
            arr += std::string(i ? ", " : "") + "{\"destination\": " + jstr(f.destination) + ", \"source\": " + jstr(f.source) +
                   ", \"digipeaters\": " + digis + ", \"control\": " + std::to_string(f.control) +
                   ", \"pid\": " + (f.has_pid ? std::to_string(f.pid) : std::string("null")) +
                   ", \"info_hex\": " + jstr(info_hex) + "}";
        }
        arr += "]";
        field("ax25_frames", arr);
    }
    field("param_search_used", r.param_search_used ? "true" : "false");
    if (r.param_search_used) field("param_search_confidence", jnum(r.param_search_confidence));
    if (r.reencode_agreement) field("reencode_agreement", jnum(*r.reencode_agreement));
    field("decode_status", jstr(status));
    field("message", jstr(bits_to_text(final_bits)));
    field("header_pattern", jstr(bits_string(r.boundaries.header_pattern)));
    field("header_pattern_source", jstr(hdr_src));
    if (r.boundaries.frame_period) field("frame_period", std::to_string(*r.boundaries.frame_period));
    if (r.raster.detected) {
        field("frame_length", std::to_string(r.raster.frame_length));
        field("raster_sync_word", jstr(bits_string(r.raster.sync_word)));
    }
    if (r.rank_analysis.detected) {
        field("code_length", std::to_string(r.rank_analysis.width));
        field("code_rate", jnum(r.rank_analysis.code_rate));
    }
    field("payload_spans", std::to_string(r.boundaries.payloads.size()), true);
    j += "}\n";
    return j;
}

std::string format_raster_text(const FrameRaster& raster) {
    if (!raster.detected) {
        return "No repeating frame structure detected: try a longer capture, or supply a "
               "candidate frame length if one is known.";
    }

    unsigned constant = 0, counter_like = 0;
    for (const auto& col : raster.columns) {
        if (col.label == ColumnLabel::Constant) ++constant;
        else if (col.label == ColumnLabel::CounterLike) ++counter_like;
    }
    const unsigned payload = static_cast<unsigned>(raster.columns.size()) - constant - counter_like;

    std::string t;
    auto line = [&](const std::string& s) { t += s + "\n"; };
    line("Frame length:         " + std::to_string(raster.frame_length) + " bits");
    line("Frames:               " + std::to_string(raster.num_frames));
    line("Sync word:            " + bits_string(raster.sync_word) + " (0x" + bits_to_hex(raster.sync_word) + ")");
    line("Header columns:       " + std::to_string(raster.header_columns.size()) + " of " +
        std::to_string(raster.frame_length) + " (" + std::to_string(constant) + " constant, " +
        std::to_string(counter_like) + " counter-like)");
    line("Payload columns:      " + std::to_string(payload));
    t += "Sync word recurrences: " + std::to_string(raster.sync_positions.size());
    return t;
}

std::string format_rank_analysis_text(const RankAnalysis& r) {
    if (!r.detected) {
        return "No rank-deficiency spike detected: try a longer capture, or supply a "
               "candidate codeword width if one is known.";
    }
    std::string t;
    auto line = [&](const std::string& s) { t += s + "\n"; };
    line("Codeword length:      " + std::to_string(r.width) + " bits");
    line("Estimated dimension:  " + printf_string("%.1f", r.rank) + " bits");
    line("Estimated code rate:  " + printf_string("%.3f", r.code_rate));
    t += "Windows analysed:     " + std::to_string(r.windows_used);
    return t;
}

}  // namespace sigapp
