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

std::string format_result_text(const PipelineResult& r, size_t max_message_chars, bool show_soft_decoding_info) {
    const FrameBoundaries& b = r.boundaries;
    std::string t;
    auto line = [&](const std::string& s) { t += s + "\n"; };

    line("File:                 " + r.file_path);
    line("Format:               " + r.file_format);
    line("Sample rate:          " + format_g(r.sample_rate) + " Hz");
    line("Num samples:          " + std::to_string(r.num_samples));
    line("Spectrogram shape:    (" + std::to_string(r.spectrogram.frames) + ", " + std::to_string(r.spectrogram.nfft) + ")");
    line("");
    line("Predicted modulation: " + r.predicted_modulation + " (confidence " + printf_string("%.2f", r.predicted_confidence) + ")");
    line("Demodulated bits:     " + std::to_string(r.demodulated_bits.size()));
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
    if (message.size() > max_message_chars) {
        const size_t total = message.size();
        message.resize(max_message_chars);
        line("Decoded message:      " + python_repr(message) + " ... (" + std::to_string(total) +
             " characters in all, the first " + std::to_string(max_message_chars) + " shown)");
    } else {
        line("Decoded message:      " + python_repr(message));
    }

    line("");
    line("Header pattern:       " + bits_string(b.header_pattern));
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

std::string format_done_status(const PipelineResult& r) {
    return "Done. Predicted modulation: " + r.predicted_modulation + " (" +
           printf_string("%.0f", r.predicted_confidence * 100.0) + "% confidence).";
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

}  // namespace sigapp
