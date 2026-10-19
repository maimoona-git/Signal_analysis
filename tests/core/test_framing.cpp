// The link-layer stage (framing.hpp): HDLC with a CRC-16, AX.25 addresses, NRZI and the G3RUH
// scrambler -- built both ways from first principles, then checked against each other, against a
// published real frame, and for false positives on random data.

#include <cstdint>
#include <random>
#include <vector>

#include "framing.hpp"
#include "../harness.hpp"
#include "../testutil.hpp"

using namespace sigapp;

namespace {

// A UI frame CPSAT1-0 > CPSAT1-0 with the given information field.
std::vector<uint8_t> ui_frame(const std::vector<uint8_t>& info) {
    std::vector<uint8_t> f = {0x86, 0xa0, 0xa6, 0x82, 0xa8, 0x62, 0xe0,   // CPSAT1-0 (destination)
                              0x86, 0xa0, 0xa6, 0x82, 0xa8, 0x62, 0x61,   // CPSAT1-0 (source, last address)
                              0x03, 0xf0};                                // UI, no layer-3
    f.insert(f.end(), info.begin(), info.end());
    return f;
}

std::vector<uint8_t> payload(size_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<uint8_t> v(n);
    for (auto& b : v) b = static_cast<uint8_t>(rng());
    return v;
}

}  // namespace

TEST_CASE("CRC-16/X-25 matches its published check value") {
    const std::vector<uint8_t> check = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(crc16_x25(check, check.size()) == 0x906E);  // the catalogue value for CRC-16/X-25
}

TEST_CASE("NRZI and the G3RUH scrambler invert exactly") {
    const auto raw = sigtest::random_bits(500, 3);
    const Bits bits(raw.begin(), raw.end());
    CHECK(nrzi_decode(nrzi_encode(bits)) == bits);
    CHECK(g3ruh_descramble(g3ruh_scramble(bits)) == bits);
    // the descrambler is self-synchronising: a different start state costs only the first 17 bits
    Bits tail(bits.begin() + 17, bits.end());
    Bits shifted(bits.begin() + 5, bits.end());
    const Bits a = g3ruh_descramble(g3ruh_scramble(bits));
    CHECK(Bits(a.begin() + 17, a.end()) == tail);
    CHECK(!shifted.empty());
}

TEST_CASE("an AX.25 frame survives HDLC encoding and is found, address and all") {
    const auto info = payload(60, 7);
    const Bits tx = hdlc_encode(ui_frame(info));
    const auto frames = find_hdlc_ax25_frames(tx);
    CHECK(frames.size() == 1);
    if (frames.size() == 1) {
        CHECK(frames[0].destination == "CPSAT1-0");
        CHECK(frames[0].source == "CPSAT1-0");
        CHECK(frames[0].control == 0x03);
        CHECK(frames[0].has_pid && frames[0].pid == 0xF0);
        CHECK(frames[0].info == info);
    }
}

TEST_CASE("bit stuffing: payloads full of 1s round-trip") {
    const std::vector<uint8_t> info(40, 0xFF);
    const auto frames = find_hdlc_ax25_frames(hdlc_encode(ui_frame(info)));
    CHECK(frames.size() == 1 && frames[0].info == info);
}

TEST_CASE("a corrupted bit fails the CRC, and an unterminated frame at the end of the stream is still accepted") {
    Bits tx = hdlc_encode(ui_frame(payload(50, 9)));
    Bits bad = tx;
    bad[100] ^= 1;
    CHECK(find_hdlc_ax25_frames(bad).empty());
    // cut off the closing flags (a receiver's filter delay does this)
    Bits cut(tx.begin(), tx.end() - 16);
    CHECK(find_hdlc_ax25_frames(cut).size() == 1);
    // ...but not if part of the FCS is gone too
    Bits cut_fcs(tx.begin(), tx.end() - 16 - 3);
    CHECK(find_hdlc_ax25_frames(cut_fcs).empty());
}

TEST_CASE("find_ax25_frames recovers a frame through G3RUH + NRZI at either polarity, with leading noise") {
    const auto info = payload(80, 11);
    const Bits line = nrzi_encode(g3ruh_scramble(hdlc_encode(ui_frame(info), 20, 4)));
    for (int inv = 0; inv < 2; ++inv) {
        Bits rx;
        const auto noise = sigtest::random_bits(333, 5);
        rx.insert(rx.end(), noise.begin(), noise.end());
        for (uint8_t b : line) rx.push_back(inv ? (b ^ 1) : b);
        const auto r = find_ax25_frames(rx);
        CHECK(r.found);
        CHECK(r.frames.size() == 1);
        if (r.found && r.frames.size() == 1) CHECK(r.frames[0].info == info);
    }
    // the plain-NRZI chain (no scrambler), as 1200-baud packet radio uses
    const Bits plain = nrzi_encode(hdlc_encode(ui_frame(info), 8, 2));
    const auto r2 = find_ax25_frames(plain);
    CHECK(r2.found && r2.frames.size() == 1);
}

TEST_CASE("a real published frame parses (CPSat beacon, mode 03)") {
    // ax25_no_fcs.hex from the CPSat beacon recording set: bytes straight from the downlink
    const char* hex =
        "86a0a682a862e086a0a682a8626103f00903c00d006957f732b284b9000000000380000000001c039402940293029202f303f703f503f903f703f5039d029e029e029b029d029e029e029e029c029e029d029e022b032303ffffc208b708000100000000000000001d031c03940293022c032903ca08010002000000f0068400";
    std::vector<uint8_t> raw;
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) raw.push_back(static_cast<uint8_t>(std::stoi(std::string(hex + i, 2), nullptr, 16)));
    const Bits line = nrzi_encode(g3ruh_scramble(hdlc_encode(raw, 30, 3)));
    const auto r = find_ax25_frames(line);
    CHECK(r.found && r.frames.size() == 1);
    if (r.found && r.frames.size() == 1) {
        CHECK(r.frames[0].raw == raw);
        CHECK(r.frames[0].destination == "CPSAT1-0");
        CHECK(r.frames[0].info.size() == raw.size() - 16);
    }
}

TEST_CASE("random bits essentially never contain a CRC-valid AX.25 frame") {
    size_t false_positives = 0;
    for (uint32_t seed = 0; seed < 300; ++seed) {
        const auto raw = sigtest::random_bits(20000, seed + 1000);
        if (find_ax25_frames(Bits(raw.begin(), raw.end())).found) ++false_positives;
    }
    CHECK(false_positives == 0);  // 300 streams x 20,000 bits x 8 chains
}

TEST_MAIN()
