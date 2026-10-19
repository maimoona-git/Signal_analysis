#include <cstdint>
#include <cstring>
#include <vector>

#include "sigmf_meta.hpp"
#include "../harness.hpp"

using namespace sigcore;

TEST_CASE("parse_sigmf_meta reads rate, datatype and first capture frequency") {
    const std::string json = R"({"global": {"core:datatype": "ci16_le", "core:sample_rate": 2400000.0},
      "captures": [{"core:sample_start": 0, "core:frequency": 433920000}], "annotations": []})";
    const auto m = parse_sigmf_meta(json);
    CHECK(m.sample_rate == std::optional<double>(2400000.0));
    CHECK(m.center_freq_hz == std::optional<double>(433920000.0));
    CHECK(m.format == std::optional<IqSampleFormat>(IqSampleFormat::Ci16));
}

TEST_CASE("parse_sigmf_meta tolerates missing, garbage and hostile input") {
    CHECK(!parse_sigmf_meta("").sample_rate);
    CHECK(!parse_sigmf_meta("not json {{{").format);
    CHECK(!parse_sigmf_meta(R"({"core:sample_rate": -5})").sample_rate);
    CHECK(!parse_sigmf_meta(R"({"core:sample_rate": "x", "core:datatype": "zz"})").sample_rate);
    CHECK(parse_sigmf_meta(R"({"core:datatype": "cu8"})").format == std::optional<IqSampleFormat>(IqSampleFormat::Cu8));
}

TEST_CASE("parse_sigmf_meta maps two-channel real data (the real SigMF logo's ri16_le) to I/Q, one channel not") {
    const auto two = parse_sigmf_meta(R"({"global":{"core:datatype":"ri16_le","core:num_channels":2,"core:sample_rate":48000}})");
    CHECK(two.format == std::optional<IqSampleFormat>(IqSampleFormat::Ci16));
    CHECK(two.sample_rate == std::optional<double>(48000.0));
    CHECK(!parse_sigmf_meta(R"({"global":{"core:datatype":"ri16_le","core:num_channels":1}})").format);
    CHECK(!parse_sigmf_meta(R"({"global":{"core:datatype":"ri16_le"}})").format);
    CHECK(!parse_sigmf_meta(R"({"global":{"core:datatype":"cf32_be"}})").format);   // big-endian: not supported
}

TEST_CASE("guess_iq_format tells float32, uint8 and int16 captures apart") {
    std::vector<uint8_t> f32(4 * 256);
    for (size_t i = 0; i < 256; ++i) {
        const float v = 0.5f * static_cast<float>(static_cast<int>(i % 7) - 3) / 3.0f + 0.1f;
        std::memcpy(&f32[4 * i], &v, 4);
    }
    CHECK(guess_iq_format(f32.data(), f32.size()) == IqSampleFormat::Cf32);

    std::vector<uint8_t> u8(1024);
    for (size_t i = 0; i < u8.size(); ++i) u8[i] = static_cast<uint8_t>(127 + static_cast<int>((i * 37) % 21) - 10);
    CHECK(guess_iq_format(u8.data(), u8.size()) == IqSampleFormat::Cu8);

    std::vector<uint8_t> i16(2048);
    for (size_t i = 0; i < i16.size() / 2; ++i) {
        const int16_t v = static_cast<int16_t>(static_cast<int>((i * 7919) % 20001) - 10000);
        std::memcpy(&i16[2 * i], &v, 2);
    }
    CHECK(guess_iq_format(i16.data(), i16.size()) == IqSampleFormat::Ci16);
}

TEST_MAIN()
