// CCSDS RS(255,223) and the RS + Viterbi concatenated chain. The two parity vectors below
// were produced by the independent libfec reference implementation (ka9q/libfec, field
// polynomial 0x187, first root 112, primitive element alpha^11) for the message
// byte[i] = (i * 37 + 11) & 255, i = 0..222.

#include <cstdint>
#include <vector>

#include "fec_ccsds.hpp"
#include "../harness.hpp"
#include "../testutil.hpp"

using namespace sigcore;

namespace {
std::vector<uint8_t> reference_message() {
    std::vector<uint8_t> m(223);
    for (size_t i = 0; i < m.size(); ++i) m[i] = static_cast<uint8_t>((i * 37 + 11) & 255);
    return m;
}
}  // namespace

TEST_CASE("CCSDS RS(255,223) parity matches the libfec reference, conventional and dual basis") {
    const std::vector<uint8_t> conv = {0xad, 0x18, 0x12, 0x37, 0x72, 0xca, 0xc8, 0xe0, 0xa8, 0x14, 0x58, 0xb1, 0xcd, 0xbe, 0x0c, 0x41, 0xba, 0x95, 0xe2, 0x3e, 0x54, 0x31, 0xbd, 0xc3, 0xf1, 0x58, 0x13, 0x5b, 0xb1, 0xd7, 0x78, 0xaa};
    const std::vector<uint8_t> dual = {0x5e, 0x90, 0x7c, 0x02, 0xde, 0xac, 0x84, 0x37, 0x2f, 0xb4, 0x52, 0x39, 0x29, 0x72, 0x77, 0x61, 0xbc, 0x4d, 0xf1, 0x0b, 0x7a, 0xc5, 0xc5, 0x04, 0x2b, 0x25, 0x8d, 0xb0, 0x17, 0xb1, 0x35, 0xed};
    const auto msg = reference_message();
    const auto c = rs_ccsds_encode_bytes(msg, false);
    const auto d = rs_ccsds_encode_bytes(msg, true);
    CHECK(c.size() == 255);
    CHECK(std::vector<uint8_t>(c.begin(), c.begin() + 223) == msg);
    CHECK(std::vector<uint8_t>(c.begin() + 223, c.end()) == conv);
    CHECK(std::vector<uint8_t>(d.begin() + 223, d.end()) == dual);
}

TEST_CASE("CCSDS RS(255,223) corrects up to 16 byte errors per block and rejects more") {
    const auto msg = reference_message();
    for (bool dual : {false, true}) {
        auto enc = rs_ccsds_encode_bytes(msg, dual);
        CHECK(rs_ccsds_decode_bytes(enc, msg.size(), dual) == msg);
        auto bad = enc;
        for (int i = 0; i < 16; ++i) bad[static_cast<size_t>(5 + i * 15)] ^= static_cast<uint8_t>(0x5A + i);
        CHECK(rs_ccsds_decode_bytes(bad, msg.size(), dual) == msg);
        for (int i = 0; i < 40; ++i) bad[static_cast<size_t>(3 + i * 6)] ^= static_cast<uint8_t>(0x33 + i);
        CHECK_THROWS(rs_ccsds_decode_bytes(bad, msg.size(), dual));
    }
    const auto longer = sigtest::random_bits(8 * 500, 9);  // multi-block round trip
    std::vector<uint8_t> bytes(500);
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>(longer[i * 8] * 7 + i);
    CHECK(rs_ccsds_decode_bytes(rs_ccsds_encode_bytes(bytes), bytes.size()) == bytes);
}

TEST_CASE("RS(255,223) + Viterbi chain round-trips and survives channel bit errors") {
    const auto msg = reference_message();
    auto coded = ccsds_concat_encode(msg);
    CHECK(ccsds_concat_decode(coded, msg.size()) == msg);
    for (size_t i = 40; i < coded.size(); i += 97) coded[i] ^= 1;  // scattered channel errors
    CHECK(ccsds_concat_decode(coded, msg.size()) == msg);
    CHECK_THROWS(ccsds_concat_encode({}));
}

TEST_MAIN()
