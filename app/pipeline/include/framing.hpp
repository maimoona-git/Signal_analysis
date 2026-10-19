#pragma once

// Gap-list follow-up (found with a real over-the-air capture, the CPSat beacon recordings): a
// link-layer stage for the commonest amateur/cubesat telemetry framing -- HDLC flags and bit
// stuffing, a CRC-16 frame check, AX.25 addresses -- optionally behind NRZI line coding and a G3RUH
// (x^17 + x^12 + 1) self-synchronising scrambler. It is a VERIFIER as well as a decoder: a frame
// whose CRC-16 checks AND whose AX.25 address field is well formed is very unlikely to be chance
// (about 2^-16 for the CRC times 2^-13 for the address structure, per candidate), so finding one is
// strong evidence the whole demodulation chain recovered the transmitted bits.
//
// Portable C++, no liquid-dsp: identical natively and under WASM.

#include <cstdint>
#include <string>
#include <vector>

#include "bits.hpp"

namespace sigapp {

struct Ax25Frame {
    std::string destination;                // "CALLSIGN-SSID"
    std::string source;
    std::vector<std::string> digipeaters;
    uint8_t control = 0;
    bool has_pid = false;                   // I and UI frames carry a protocol id
    uint8_t pid = 0;
    std::vector<uint8_t> info;              // the information field
    std::vector<uint8_t> raw;               // the whole frame, flags and FCS removed
    size_t bit_offset = 0;                  // where the frame's opening flag sits in the searched stream
};

struct FramingResult {
    bool found = false;
    // Which chain recovered the frames, e.g. "NRZI + G3RUH descrambling" or
    // "NRZI + G3RUH descrambling (polarity inverted)".
    std::string chain;
    std::vector<Ax25Frame> frames;
};

// CRC-16/X-25 (HDLC FCS): reflected polynomial 0x8408, init 0xFFFF, final xor 0xFFFF.
uint16_t crc16_x25(const std::vector<uint8_t>& data, size_t length);

// Line-coding primitives, exposed for tests. NRZI uses the AX.25 convention (a 0 is a transition,
// a 1 is no transition); the descrambler is G3RUH's x^17 + x^12 + 1.
Bits nrzi_decode(const Bits& bits);
Bits nrzi_encode(const Bits& bits);
Bits g3ruh_descramble(const Bits& bits);
Bits g3ruh_scramble(const Bits& bits);

// HDLC-frames `bits` (as transmitted: flags 01111110, zero-stuffing after five 1s, bytes LSB first)
// and returns every frame with a valid CRC-16 whose AX.25 address field parses. A frame still open
// when the stream ends is accepted too (a receiver's filter delay routinely cuts the closing flag).
std::vector<Ax25Frame> find_hdlc_ax25_frames(const Bits& bits);

// Tries the usual chains on a demodulated stream: raw, NRZI, NRZI + G3RUH, G3RUH + NRZI, each at
// both polarities, and returns the chain that yields the most frames (ties: the simplest chain).
FramingResult find_ax25_frames(const Bits& demodulated_bits);

// Builds a transmitted-order bit stream for one AX.25 frame (no FCS in `frame_bytes`): flags, the
// stuffed frame with its FCS. Used by the tests and the demo generator.
Bits hdlc_encode(const std::vector<uint8_t>& frame_bytes, size_t leading_flags = 4, size_t trailing_flags = 2);

}  // namespace sigapp
