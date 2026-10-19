#include "framing.hpp"

#include <algorithm>
#include <array>

namespace sigapp {

uint16_t crc16_x25(const std::vector<uint8_t>& data, size_t length) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length && i < data.size(); ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) crc = (crc & 1) ? static_cast<uint16_t>((crc >> 1) ^ 0x8408) : static_cast<uint16_t>(crc >> 1);
    }
    return static_cast<uint16_t>(crc ^ 0xFFFF);
}

Bits nrzi_decode(const Bits& bits) {
    Bits out(bits.size());
    uint8_t prev = 0;
    for (size_t i = 0; i < bits.size(); ++i) {
        out[i] = bits[i] == prev ? 1 : 0;
        prev = bits[i];
    }
    return out;
}

Bits nrzi_encode(const Bits& bits) {
    Bits out(bits.size());
    uint8_t level = 0;
    for (size_t i = 0; i < bits.size(); ++i) {
        if (bits[i] == 0) level ^= 1;  // a 0 is a transition
        out[i] = level;
    }
    return out;
}

Bits g3ruh_descramble(const Bits& bits) {
    Bits out(bits.size());
    for (size_t i = 0; i < bits.size(); ++i) {
        const uint8_t a = i >= 12 ? bits[i - 12] : 0;
        const uint8_t c = i >= 17 ? bits[i - 17] : 0;
        out[i] = bits[i] ^ a ^ c;
    }
    return out;
}

Bits g3ruh_scramble(const Bits& bits) {
    Bits out(bits.size());
    for (size_t i = 0; i < bits.size(); ++i) {
        const uint8_t a = i >= 12 ? out[i - 12] : 0;
        const uint8_t c = i >= 17 ? out[i - 17] : 0;
        out[i] = bits[i] ^ a ^ c;
    }
    return out;
}

namespace {

// One AX.25 address: 6 callsign characters, each shifted left one bit, then an SSID byte whose low
// bit marks the last address in the field.
bool parse_address(const std::vector<uint8_t>& f, size_t at, std::string& out, bool& last) {
    if (at + 7 > f.size()) return false;
    std::string call;
    for (size_t i = 0; i < 6; ++i) {
        if (f[at + i] & 1) return false;  // only the SSID byte may end the address field
        const char c = static_cast<char>(f[at + i] >> 1);
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ')) return false;
        if (c != ' ') call += c;
    }
    if (call.empty()) return false;
    const uint8_t ssid_byte = f[at + 6];
    if ((ssid_byte & 0x60) != 0x60) return false;  // the two reserved bits are 1 in AX.25
    const int ssid = (ssid_byte >> 1) & 0x0F;
    out = call + "-" + std::to_string(ssid);
    last = (ssid_byte & 1) != 0;
    return true;
}

bool parse_ax25(const std::vector<uint8_t>& raw, Ax25Frame& frame) {
    // destination, source, up to 8 digipeaters, then control (and a PID for I/UI frames)
    if (raw.size() < 15) return false;
    std::vector<std::string> addresses;
    size_t at = 0;
    bool last = false;
    while (!last) {
        if (addresses.size() >= 10) return false;
        std::string a;
        if (!parse_address(raw, at, a, last)) return false;
        addresses.push_back(a);
        at += 7;
    }
    if (addresses.size() < 2 || at >= raw.size()) return false;
    frame.destination = addresses[0];
    frame.source = addresses[1];
    frame.digipeaters.assign(addresses.begin() + 2, addresses.end());
    frame.control = raw[at++];
    const bool is_i = (frame.control & 0x01) == 0;
    const bool is_ui = (frame.control & 0xEF) == 0x03;
    frame.has_pid = is_i || is_ui;
    if (frame.has_pid) {
        if (at >= raw.size()) return false;
        frame.pid = raw[at++];
    }
    frame.info.assign(raw.begin() + static_cast<long>(at), raw.end());
    frame.raw = raw;
    return true;
}

// Unstuffs the bits between two flags; false if it is not a clean frame (an abort, an odd bit count).
bool unstuff(const Bits& bits, size_t begin, size_t end, std::vector<uint8_t>& bytes) {
    Bits out;
    out.reserve(end - begin);
    int ones = 0;
    for (size_t i = begin; i < end; ++i) {
        if (bits[i]) {
            if (++ones >= 7) return false;
            out.push_back(1);
        } else {
            if (ones == 5) { ones = 0; continue; }  // a stuffed zero
            if (ones == 6) return false;            // 0111111 0 would be a flag
            ones = 0;
            out.push_back(0);
        }
    }
    if (out.size() % 8 != 0 || out.size() < 8 * 17) return false;
    bytes.assign(out.size() / 8, 0);
    for (size_t i = 0; i < out.size(); ++i)
        if (out[i]) bytes[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
    return true;
}

}  // namespace

std::vector<Ax25Frame> find_hdlc_ax25_frames(const Bits& bits) {
    std::vector<Ax25Frame> frames;
    const std::array<uint8_t, 8> flag = {0, 1, 1, 1, 1, 1, 1, 0};
    std::vector<size_t> flags;
    for (size_t i = 0; i + 8 <= bits.size(); ++i) {
        if (std::equal(flag.begin(), flag.end(), bits.begin() + static_cast<long>(i))) {
            flags.push_back(i);
            i += 7;  // flags do not overlap
        }
    }
    auto try_span = [&](size_t open_flag, size_t body_begin, size_t body_end) {
        std::vector<uint8_t> bytes;
        if (!unstuff(bits, body_begin, body_end, bytes)) return;
        const size_t n = bytes.size();
        const uint16_t fcs = static_cast<uint16_t>(bytes[n - 2] | (bytes[n - 1] << 8));
        if (crc16_x25(bytes, n - 2) != fcs) return;
        bytes.resize(n - 2);
        Ax25Frame f;
        if (!parse_ax25(bytes, f)) return;
        f.bit_offset = open_flag;
        frames.push_back(std::move(f));
    };
    for (size_t k = 0; k + 1 < flags.size(); ++k) {
        if (flags[k + 1] <= flags[k] + 8) continue;  // back-to-back flags: an empty frame
        try_span(flags[k], flags[k] + 8, flags[k + 1]);
    }
    // a frame still open at the end of the stream (its closing flag cut off by filter delay)
    if (!flags.empty() && bits.size() > flags.back() + 8) try_span(flags.back(), flags.back() + 8, bits.size());
    return frames;
}

FramingResult find_ax25_frames(const Bits& demodulated) {
    FramingResult best;
    if (demodulated.size() < 8 * 20) return best;
    struct Chain { const char* name; int kind; };
    const Chain chains[] = {{"HDLC", 0}, {"NRZI", 1}, {"NRZI + G3RUH descrambling", 2}, {"G3RUH descrambling + NRZI", 3}};
    for (const Chain& c : chains) {
        for (int inv = 0; inv < 2; ++inv) {
            Bits b = demodulated;
            if (inv) for (auto& x : b) x ^= 1;
            switch (c.kind) {
                case 1: b = nrzi_decode(b); break;
                case 2: b = g3ruh_descramble(nrzi_decode(b)); break;
                case 3: b = nrzi_decode(g3ruh_descramble(b)); break;
                default: break;
            }
            auto frames = find_hdlc_ax25_frames(b);
            if (frames.size() > best.frames.size()) {
                best.found = true;
                best.chain = std::string(c.name) + (inv ? " (polarity inverted)" : "");
                best.frames = std::move(frames);
            }
        }
    }
    return best;
}

Bits hdlc_encode(const std::vector<uint8_t>& frame, size_t leading_flags, size_t trailing_flags) {
    std::vector<uint8_t> bytes = frame;
    const uint16_t fcs = crc16_x25(bytes, bytes.size());
    bytes.push_back(static_cast<uint8_t>(fcs & 0xFF));
    bytes.push_back(static_cast<uint8_t>(fcs >> 8));
    const Bits flag = {0, 1, 1, 1, 1, 1, 1, 0};
    Bits out;
    for (size_t i = 0; i < leading_flags; ++i) out.insert(out.end(), flag.begin(), flag.end());
    int ones = 0;
    for (uint8_t byte : bytes) {
        for (int k = 0; k < 8; ++k) {
            const uint8_t bit = (byte >> k) & 1;
            out.push_back(bit);
            if (bit) {
                if (++ones == 5) { out.push_back(0); ones = 0; }
            } else {
                ones = 0;
            }
        }
    }
    for (size_t i = 0; i < trailing_flags; ++i) out.insert(out.end(), flag.begin(), flag.end());
    return out;
}

}  // namespace sigapp
