// Punctured Viterbi (2/3, 3/4) and RS(255,223): round trips, length arithmetic, and error
// correction up to each code's stated limit and failure beyond it.

#include <cstdint>
#include <vector>

#include "fec_extra.hpp"
#include "../harness.hpp"
#include "../testutil.hpp"

using namespace sigcore;

TEST_CASE("punctured Viterbi round-trips and has the stated code rate") {
    for (auto rate : {std::pair<unsigned, unsigned>{2, 3}, std::pair<unsigned, unsigned>{3, 4}}) {
        for (size_t n : {40u, 97u, 256u}) {
            const auto bits = sigtest::random_bits(n, static_cast<unsigned>(n));
            const std::vector<uint8_t> msg(bits.begin(), bits.end());
            const auto coded = viterbi_punctured_encode(msg, rate.first, rate.second);
            CHECK(coded.size() == viterbi_punctured_length(n, rate.first, rate.second));
            CHECK(viterbi_punctured_decode(coded, n, rate.first, rate.second) == msg);
            if (n >= 256) {  // the 6 flush bits distort the rate of a short message
                const double actual = static_cast<double>(n) / static_cast<double>(coded.size());
                CHECK_NEAR(actual, static_cast<double>(rate.first) / rate.second, 0.05);
            }
        }
    }
    CHECK_THROWS(viterbi_punctured_encode({1, 0, 1}, 5, 6));
}

TEST_CASE("punctured Viterbi corrects scattered bit errors") {
    const auto bits = sigtest::random_bits(300, 11);
    const std::vector<uint8_t> msg(bits.begin(), bits.end());
    auto coded = viterbi_punctured_encode(msg, 2, 3);
    for (size_t i = 20; i < coded.size(); i += 90) coded[i] ^= 1;
    CHECK(viterbi_punctured_decode(coded, msg.size(), 2, 3) == msg);
}

TEST_CASE("RS(255,223) round-trips multi-block messages") {
    std::vector<uint8_t> msg(500);
    for (size_t i = 0; i < msg.size(); ++i) msg[i] = static_cast<uint8_t>(i * 7 + 3);
    const auto enc = rs255_encode_bytes(msg);
    CHECK(enc.size() == 3 * 255);  // ceil(500/223) blocks
    CHECK(rs255_decode_bytes(enc, msg.size()) == msg);
    CHECK_THROWS(rs255_decode_bytes(std::vector<uint8_t>(100), 10));
}

TEST_CASE("RS(255,223) corrects 16 byte errors per block and rejects 17") {
    std::vector<uint8_t> msg(223);
    for (size_t i = 0; i < msg.size(); ++i) msg[i] = static_cast<uint8_t>(i * 31 + 5);
    auto enc = rs255_encode_bytes(msg);
    auto bad = enc;
    for (int k = 0; k < 16; ++k) bad[static_cast<size_t>(k) * 15 + 2] ^= static_cast<uint8_t>(0x40 + k);
    CHECK(rs255_decode_bytes(bad, msg.size()) == msg);
    auto worse = enc;
    for (int k = 0; k < 17; ++k) worse[static_cast<size_t>(k) * 14 + 1] ^= static_cast<uint8_t>(0x55 + k);
    bool threw = false;
    std::vector<uint8_t> got;
    try { got = rs255_decode_bytes(worse, msg.size()); } catch (const std::exception&) { threw = true; }
    CHECK(threw || got != msg);  // beyond t=16 it must not silently claim the original back
}

TEST_MAIN()
