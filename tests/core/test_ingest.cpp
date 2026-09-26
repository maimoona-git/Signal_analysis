// Phase 4: in-memory file ingestion (the C++ half of the Embind "bytes in"
// path). Input is untrusted browser data, so besides the happy paths this
// checks malformed/truncated/corrupted files fail cleanly instead of crashing.

#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "ingest.hpp"
#include "iq_io.hpp"
#include "wav_io.hpp"
#include "../harness.hpp"
#include "../testutil.hpp"

using namespace sigcore;
using namespace sigtest;
using Bytes = std::vector<uint8_t>;

namespace {

void put_u16(Bytes& b, uint16_t v) { b.push_back(v & 0xFF); b.push_back(v >> 8); }
void put_u32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back((v >> (8 * i)) & 0xFF); }
void put_tag(Bytes& b, const char* t) { b.insert(b.end(), t, t + 4); }

Bytes fmt_chunk(uint16_t format, uint16_t channels, uint32_t rate, uint16_t bits) {
    Bytes c;
    put_tag(c, "fmt ");
    put_u32(c, 16);
    put_u16(c, format);
    put_u16(c, channels);
    put_u32(c, rate);
    put_u32(c, rate * channels * bits / 8);
    put_u16(c, static_cast<uint16_t>(channels * bits / 8));
    put_u16(c, bits);
    return c;
}

Bytes data_chunk(const Bytes& payload, uint32_t declared_size) {
    Bytes c;
    put_tag(c, "data");
    put_u32(c, declared_size);
    c.insert(c.end(), payload.begin(), payload.end());
    if (payload.size() % 2 && declared_size == payload.size()) c.push_back(0);  // RIFF pad byte
    return c;
}

Bytes riff(const std::vector<Bytes>& chunks) {
    Bytes body;
    put_tag(body, "WAVE");
    for (const auto& c : chunks) body.insert(body.end(), c.begin(), c.end());
    Bytes out;
    put_tag(out, "RIFF");
    put_u32(out, static_cast<uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

Bytes pcm16(const std::vector<int16_t>& v) {
    Bytes b;
    for (int16_t s : v) put_u16(b, static_cast<uint16_t>(s));
    return b;
}

Bytes float32(const std::vector<float>& v) {
    Bytes b(v.size() * 4);
    std::memcpy(b.data(), v.data(), b.size());
    return b;
}

Bytes mono16_wav(const std::vector<int16_t>& s, uint32_t rate = 8000) {
    const Bytes p = pcm16(s);
    return riff({fmt_chunk(1, 1, rate, 16), data_chunk(p, static_cast<uint32_t>(p.size()))});
}

SignalData parse(const Bytes& b) { return parse_wav(b.data(), b.size(), "test.wav"); }

}  // namespace

TEST_CASE("parse_wav: 16-bit mono is a real signal") {
    const auto sig = parse(mono16_wav({0, 16384, -16384, -32768}, 44100));
    CHECK(sig.num_samples() == 4);
    CHECK_NEAR(sig.sample_rate, 44100.0, 0.0);
    CHECK(sig.format == "wav_mono_real");
    CHECK_NEAR(sig.samples[1].real(), 0.5, 1e-6);
    CHECK_NEAR(sig.samples[2].real(), -0.5, 1e-6);
    CHECK_NEAR(sig.samples[3].real(), -1.0, 1e-6);
    CHECK_NEAR(sig.samples[1].imag(), 0.0, 0.0);
}

TEST_CASE("parse_wav: 32-bit float stereo is I/Q") {
    const Bytes p = float32({0.25f, -0.5f, 1.0f, 0.125f});
    const auto sig = parse(riff({fmt_chunk(3, 2, 48000, 32), data_chunk(p, 16)}));
    CHECK(sig.num_samples() == 2);
    CHECK(sig.format == "wav_stereo_iq");
    CHECK_NEAR(sig.samples[0].real(), 0.25, 0.0);
    CHECK_NEAR(sig.samples[0].imag(), -0.5, 0.0);
    CHECK_NEAR(sig.samples[1].real(), 1.0, 0.0);
    CHECK_NEAR(sig.samples[1].imag(), 0.125, 0.0);
}

TEST_CASE("parse_wav: 8-bit PCM is unsigned, centred on 128") {
    const Bytes p = {128, 255, 0, 192};
    const auto sig = parse(riff({fmt_chunk(1, 1, 8000, 8), data_chunk(p, 4)}));
    CHECK(sig.num_samples() == 4);
    CHECK_NEAR(sig.samples[0].real(), 0.0, 1e-6);
    CHECK_NEAR(sig.samples[1].real(), 127.0 / 128.0, 1e-6);
    CHECK_NEAR(sig.samples[2].real(), -1.0, 1e-6);
    CHECK_NEAR(sig.samples[3].real(), 0.5, 1e-6);
}

TEST_CASE("parse_wav: skips unknown chunks (including odd-sized, padded ones)") {
    Bytes list;
    put_tag(list, "LIST");
    put_u32(list, 5);
    list.insert(list.end(), {'a', 'b', 'c', 'd', 'e', 0});  // 5 bytes + pad
    const Bytes p = pcm16({1000, -1000});
    const auto sig = parse(riff({list, fmt_chunk(1, 1, 8000, 16), list, data_chunk(p, 4)}));
    CHECK(sig.num_samples() == 2);
    CHECK_NEAR(sig.samples[0].real(), 1000.0 / 32768.0, 1e-6);
}

TEST_CASE("parse_wav: data chunk before fmt chunk is accepted") {
    const Bytes p = pcm16({3277});
    const auto sig = parse(riff({data_chunk(p, 2), fmt_chunk(1, 1, 8000, 16)}));
    CHECK(sig.num_samples() == 1);
    CHECK_NEAR(sig.samples[0].real(), 3277.0 / 32768.0, 1e-6);
}

TEST_CASE("parse_wav: a data chunk claiming more bytes than exist is clamped to whole frames") {
    // Streamed/truncated files: size field 0xFFFFFFFF, and a trailing half frame.
    const Bytes p = pcm16({100, 200, 300});
    Bytes bytes = riff({fmt_chunk(1, 1, 8000, 16), data_chunk(p, 0xFFFFFFFFu)});
    bytes.push_back(0x7F);  // dangling half-frame
    const auto sig = parse(bytes);
    CHECK(sig.num_samples() == 3);
    CHECK_NEAR(sig.samples[2].real(), 300.0 / 32768.0, 1e-6);
}

TEST_CASE("parse_wav: empty data chunk is a valid zero-sample signal") {
    const auto sig = parse(riff({fmt_chunk(1, 1, 8000, 16), data_chunk({}, 0)}));
    CHECK(sig.num_samples() == 0);
}

TEST_CASE("parse_wav: rejects malformed files with std::runtime_error") {
    CHECK_THROWS(parse({}));
    CHECK_THROWS(parse({'R', 'I', 'F', 'F'}));
    CHECK_THROWS(parse(random_bytes(64, 1)));
    Bytes not_wave = mono16_wav({1, 2});
    not_wave[8] = 'X';
    CHECK_THROWS(parse(not_wave));
    const Bytes p = pcm16({1, 2});
    CHECK_THROWS(parse(riff({data_chunk(p, 4)})));                                  // no fmt
    CHECK_THROWS(parse(riff({fmt_chunk(1, 1, 8000, 16)})));                         // no data
    CHECK_THROWS(parse(riff({fmt_chunk(1, 3, 8000, 16), data_chunk(p, 4)})));       // 3 channels
    CHECK_THROWS(parse(riff({fmt_chunk(2, 1, 8000, 16), data_chunk(p, 4)})));       // ADPCM
    CHECK_THROWS(parse(riff({fmt_chunk(1, 1, 8000, 24), data_chunk(Bytes(6, 1), 6)})));  // 24-bit
    CHECK_THROWS(parse(riff({fmt_chunk(1, 1, 8000, 4), data_chunk(p, 4)})));        // < 8 bits
    CHECK_THROWS(parse(riff({fmt_chunk(1, 1, 8000, 16), data_chunk(Bytes(3, 1), 3)})));  // misaligned
    Bytes small_fmt;
    put_tag(small_fmt, "fmt ");
    put_u32(small_fmt, 8);
    small_fmt.insert(small_fmt.end(), 8, 0);
    CHECK_THROWS(parse(riff({small_fmt, data_chunk(p, 4)})));                       // fmt < 16 bytes
    Bytes huge_fmt = fmt_chunk(1, 1, 8000, 16);
    huge_fmt[4] = 0xF0; huge_fmt[5] = 0xFF; huge_fmt[6] = 0xFF; huge_fmt[7] = 0x7F;
    CHECK_THROWS(parse(riff({huge_fmt, data_chunk(p, 4)})));                        // fmt overruns file
}

TEST_CASE("parse_wav: an absurd chunk size does not allocate or crash") {
    Bytes junk;
    put_tag(junk, "LIST");
    put_u32(junk, 0xFFFFFFF0u);
    CHECK_THROWS(parse(riff({junk})));  // runs off the end, then "missing fmt or data"
}

TEST_CASE("parse_wav: every truncation and 2000 corruptions parse or throw, never crash") {
    const Bytes good = riff({fmt_chunk(1, 2, 8000, 16), data_chunk(pcm16({1, -2, 3, -4, 5, -6, 7, -8}), 16)});
    for (size_t len = 0; len <= good.size(); ++len) {
        try { (void)parse_wav(good.data(), len, "trunc"); } catch (const std::exception&) {}
    }
    Rng rng(1234);
    for (int i = 0; i < 2000; ++i) {
        Bytes bad = good;
        const int flips = 1 + static_cast<int>(rng.next() % 4);
        for (int f = 0; f < flips; ++f) bad[rng.next() % bad.size()] = static_cast<uint8_t>(rng.next());
        try { (void)parse_wav(bad.data(), bad.size(), "fuzz"); } catch (const std::exception&) {}
    }
}

TEST_CASE("parse_iq: interleaved float32 I/Q") {
    const Bytes p = float32({1.0f, 2.0f, -3.5f, 0.25f});
    const auto sig = parse_iq(p.data(), p.size(), 2.4e6, "t.iq");
    CHECK(sig.num_samples() == 2);
    CHECK(sig.format == "iq_cf32");
    CHECK_NEAR(sig.sample_rate, 2.4e6, 0.0);
    CHECK(sig.samples[0] == std::complex<float>(1.0f, 2.0f));
    CHECK(sig.samples[1] == std::complex<float>(-3.5f, 0.25f));
}

TEST_CASE("parse_iq: rejects bad sample rate and misaligned size; empty is zero samples") {
    const Bytes p = float32({1.0f, 2.0f});
    CHECK_THROWS(parse_iq(p.data(), p.size(), 0.0, "t"));
    CHECK_THROWS(parse_iq(p.data(), p.size(), -1.0, "t"));
    CHECK_THROWS(parse_iq(p.data(), p.size() - 1, 8000.0, "t"));
    CHECK(parse_iq(p.data(), 0, 8000.0, "t").num_samples() == 0);
}

TEST_CASE("ingest_bytes: routes by content first, then by extension") {
    const Bytes wav = mono16_wav({100, 200});
    const Bytes iq = float32({1.0f, 0.0f, 0.0f, 1.0f});

    CHECK(detect_file_kind(wav.data(), wav.size(), "capture.bin") == FileKind::Wav);   // magic wins
    CHECK(detect_file_kind(iq.data(), iq.size(), "capture.IQ") == FileKind::Iq);
    CHECK(detect_file_kind(iq.data(), iq.size(), "capture") == FileKind::Iq);
    CHECK(detect_file_kind(iq.data(), iq.size(), "x.WAV") == FileKind::Wav);           // extension, no magic

    CHECK(ingest_bytes(wav.data(), wav.size(), "a.bin", 0.0).num_samples() == 2);      // WAV ignores rate
    CHECK(ingest_bytes(iq.data(), iq.size(), "a.iq", 1000.0).num_samples() == 2);
    CHECK_THROWS(ingest_bytes(iq.data(), iq.size(), "a.iq", 0.0));                     // IQ needs a rate
    CHECK_THROWS(ingest_bytes(iq.data(), iq.size(), "mislabelled.wav", 1000.0));       // .wav but not RIFF
}

TEST_CASE("load_wav / load_iq still work from a path and name it in errors") {
    const std::string wav_path = "phase4_ingest.wav", iq_path = "phase4_ingest.iq";
    const Bytes wav = mono16_wav({0, 16384});
    const Bytes iq = float32({1.0f, -1.0f});
    {
        std::ofstream(wav_path, std::ios::binary).write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
        std::ofstream(iq_path, std::ios::binary).write(reinterpret_cast<const char*>(iq.data()), static_cast<std::streamsize>(iq.size()));
    }
    CHECK(load_wav(wav_path).num_samples() == 2);
    CHECK(load_iq(iq_path, 8000.0).samples[0] == std::complex<float>(1.0f, -1.0f));
    std::remove(wav_path.c_str());
    std::remove(iq_path.c_str());

    std::string msg;
    try { load_wav("/no/such/file.wav"); } catch (const std::exception& e) { msg = e.what(); }
    CHECK(msg.find("/no/such/file.wav") != std::string::npos);
    msg.clear();
    try { load_iq("/no/such/file.iq", 8000.0); } catch (const std::exception& e) { msg = e.what(); }
    CHECK(msg.find("/no/such/file.iq") != std::string::npos);
}

// The native app's own fixture captures (tests/data/fixtures, from its data/fixtures): real
// files written by other tools, loaded from disk the way the native tests do.
#ifndef SIG_TEST_DATA_DIR
#error "SIG_TEST_DATA_DIR must point at tests/data"
#endif
namespace {
std::string fixture(const char* name) { return std::string(SIG_TEST_DATA_DIR) + "/fixtures/" + name; }
Bytes file_bytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
}  // namespace

TEST_CASE("fixture: mono WAV loads as a real signal of 400 samples at 8 kHz") {
    const auto sig = load_wav(fixture("tone_mono_8k.wav"));
    CHECK_NEAR(sig.sample_rate, 8000.0, 0.0);
    CHECK(sig.format == "wav_mono_real");
    CHECK(sig.num_samples() == 400);
    CHECK_NEAR(sig.duration_sec(), 0.05, 1e-3 * 0.05);
    for (const auto& v : sig.samples) CHECK(v.imag() == 0.0f);
}

TEST_CASE("fixture: stereo WAV loads as I/Q, with a non-zero imaginary part") {
    const auto sig = load_wav(fixture("tone_stereo_iq_8k.wav"));
    CHECK_NEAR(sig.sample_rate, 8000.0, 0.0);
    CHECK(sig.format == "wav_stereo_iq");
    CHECK(sig.num_samples() == 400);
    bool any_imag = false;
    for (const auto& v : sig.samples) any_imag |= (v.imag() != 0.0f);
    CHECK(any_imag);
}

TEST_CASE("fixture: raw .IQ loads with the caller's sample rate, which must be positive") {
    const auto sig = load_iq(fixture("tone_cf32_8k.iq"), 8000.0);
    CHECK_NEAR(sig.sample_rate, 8000.0, 0.0);
    CHECK(sig.format == "iq_cf32");
    CHECK(sig.num_samples() == 400);
    CHECK_THROWS(load_iq(fixture("tone_cf32_8k.iq"), 0.0));
    CHECK_THROWS(load_iq(fixture("tone_cf32_8k.iq"), -1.0));
}

TEST_CASE("fixture: a missing file is an error that names the file") {
    std::string msg;
    try { load_wav(fixture("does_not_exist.wav")); } catch (const std::exception& e) { msg = e.what(); }
    CHECK(msg.find("does_not_exist.wav") != std::string::npos);
    CHECK_THROWS(load_iq(fixture("does_not_exist.iq"), 8000.0));
}

TEST_CASE("fixture: loading from disk and parsing the same bytes in memory agree exactly") {
    for (const char* name : {"tone_mono_8k.wav", "tone_stereo_iq_8k.wav"}) {
        const auto from_disk = load_wav(fixture(name));
        const auto bytes = file_bytes(fixture(name));
        const auto from_memory = parse_wav(bytes.data(), bytes.size(), name);
        CHECK(from_disk.samples == from_memory.samples);
        CHECK(from_disk.sample_rate == from_memory.sample_rate && from_disk.format == from_memory.format);
        const auto via_ingest = ingest_bytes(bytes.data(), bytes.size(), name, 0.0);   // the browser's entry point
        CHECK(via_ingest.samples == from_disk.samples);
    }
    const auto iq_bytes = file_bytes(fixture("tone_cf32_8k.iq"));
    CHECK(load_iq(fixture("tone_cf32_8k.iq"), 8000.0).samples == parse_iq(iq_bytes.data(), iq_bytes.size(), 8000.0).samples);
}

TEST_MAIN()
