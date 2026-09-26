// Phase 6: the by-name dispatchers (fec, interleave, demod) and file loading,
// compared with the ORIGINAL app/fec.py, app/interleave.py, app/io.py: recorded
// outputs for every scheme and size, and every case where the original raises.

#include <cmath>
#include <complex>
#include <string>
#include <vector>

#include "demod_dispatch.hpp"
#include "fec_dispatch.hpp"
#include "interleave_dispatch.hpp"
#include "signal_io.hpp"
#include "../harness.hpp"
#include "../record_file.hpp"
#include "../testutil.hpp"

using namespace sigapp;
using namespace sigtest;

#ifndef SIG_TEST_DATA_DIR
#error "SIG_TEST_DATA_DIR must point at tests/data"
#endif

namespace {

std::string data_path(const std::string& rel) { return std::string(SIG_TEST_DATA_DIR) + "/pipeline/" + rel; }

InterleaverSpec spec_of(const std::string& kind, const std::string& params) {
    InterleaverSpec s;
    s.kind = kind;
    for (const auto& [k, v] : params_of(params)) {
        const auto value = static_cast<uint32_t>(to_int(v));
        if (k == "rows") s.rows = value;
        else if (k == "cols") s.cols = value;
        else if (k == "seed") s.seed = value;
        else if (k == "branches") s.branches = value;
        else if (k == "delay_increment") s.delay_increment = value;
    }
    return s;
}

}  // namespace

TEST_CASE("fec dispatcher matches the original: every scheme, size, and error pattern") {
    const auto recs = read_records(data_path("fec.rec"));
    Mismatches m;
    size_t errors_expected = 0;
    for (const auto& r : recs) {
        const std::string label = field(r, "op") + " " + field(r, "scheme") + " n=" + std::to_string(field(r, "bits").size());
        const auto bits = bits_of(field(r, "bits"));
        auto run = [&]() -> Bits {
            return field(r, "op") == "encode" ? fec_encode(bits, field(r, "scheme"))
                                              : fec_decode(bits, field(r, "scheme"), static_cast<size_t>(to_int(field(r, "msg_len"))));
        };
        if (has(r, "error")) {
            ++errors_expected;
            bool threw = false;
            try { run(); } catch (const std::exception&) { threw = true; }
            m.expect(threw, label, "original raised " + field(r, "error") + " but this did not throw");
            continue;
        }
        try {
            m.equal(run(), bits_of(field(r, "out")), label + (has(r, "case") ? " " + field(r, "case") : ""), "output");
        } catch (const std::exception& e) {
            m.expect(false, label, std::string("threw unexpectedly: ") + e.what());
        }
    }
    CHECK(recs.size() == 251);
    CHECK(errors_expected >= 8);
    m.finish(recs.size());
}

TEST_CASE("interleave dispatcher matches the original: every kind, parameter set, and error") {
    const auto recs = read_records(data_path("interleave.rec"));
    Mismatches m;
    for (const auto& r : recs) {
        const std::string label = field(r, "op") + " " + field(r, "kind") + " " + field(r, "params");
        const auto bits = bits_of(field(r, "bits"));
        const auto spec = spec_of(field(r, "kind"), field(r, "params"));
        auto run = [&]() -> Bits { return field(r, "op") == "interleave" ? interleave(bits, spec) : deinterleave(bits, spec); };
        if (has(r, "error")) {
            bool threw = false;
            try { run(); } catch (const std::exception&) { threw = true; }
            m.expect(threw, label, "original raised " + field(r, "error") + " but this did not throw");
            continue;
        }
        try {
            m.equal(run(), bits_of(field(r, "out")), label, "output");
        } catch (const std::exception& e) {
            m.expect(false, label, std::string("threw unexpectedly: ") + e.what());
        }
    }
    CHECK(recs.size() == 44);
    m.finish(recs.size());
}

TEST_CASE("dispatchers name their errors") {
    auto message_of = [](auto&& fn) { try { fn(); } catch (const std::exception& e) { return std::string(e.what()); } return std::string(); };
    CHECK(message_of([] { fec_encode({1, 0}, "turbo"); }).find("unknown FEC scheme: 'turbo'") != std::string::npos);
    CHECK(message_of([] { interleave({1, 0}, {"zigzag", {}, {}, {}, {}, {}}); }).find("unknown interleaver: 'zigzag'") != std::string::npos);
    CHECK(message_of([] { interleave({1, 0}, InterleaverSpec{"block", 2u, {}, {}, {}, {}}); }).find("requires parameter 'cols'") != std::string::npos);
    CHECK(message_of([] { demodulate({}, "morse"); }).find("unknown modulation scheme: 'morse'") != std::string::npos);
}

TEST_CASE("demod dispatcher: every scheme name, case-insensitive, FSK defaults") {
    const auto bits = random_bits(840, 5);
    for (const char* name : {"bpsk", "qpsk", "psk8", "psk16", "psk32", "psk64", "qam4", "qam16", "qam32", "qam64",
                             "qam128", "qam256", "ask2", "ask4", "ask8", "ask16", "ook", "BPSK", "Qam16"}) {
        const auto recovered = demodulate(modulate(bits, name), name);
        CHECK(std::equal(bits.begin(), bits.end(), recovered.begin()));
    }
    // FSK: defaults are 1 bit/symbol, 8 samples/symbol, bandwidth 0.25
    const DemodParams defaults;
    CHECK(defaults.bits_per_symbol == 1 && defaults.samples_per_symbol == 8 && defaults.bandwidth == 0.25f);
    const auto fsk = modulate(bits, "fsk");
    CHECK(fsk.size() == bits.size() * 8);
    CHECK(demodulate(fsk, "fsk") == bits);
    DemodParams two{2, 16, 0.3f};
    CHECK(demodulate(modulate(bits, "FSK", two), "fsk", two) == bits);
    CHECK_THROWS(linear_scheme_from_name("fsk"));  // fsk is not a linear scheme
}

TEST_CASE("load_signal: format by extension, rates, and errors, like io.py") {
    auto wav_bytes = [] {
        std::vector<uint8_t> b = {'R','I','F','F',40,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,1,0,0x40,0x1F,0,0,0x80,0x3E,0,0,2,0,16,0,'d','a','t','a',4,0,0,0,0,0,0,0x40};
        return b;
    }();
    const auto w = load_signal(wav_bytes.data(), wav_bytes.size(), "cap.WAV", 999.0);   // extension is case-insensitive
    CHECK(w.sample_rate == 8000.0);                                                       // header wins over the argument
    CHECK(w.num_samples == 2 && w.format == "wav_mono_real" && w.path == "cap.WAV");

    const std::vector<float> iq = {1, 2, 3, 4};
    const auto* p = reinterpret_cast<const uint8_t*>(iq.data());
    CHECK(load_signal(p, 16, "x.iq", 4000.0).sample_rate == 4000.0);
    CHECK(load_signal(p, 16, "x.iq").sample_rate == kDefaultIqSampleRate);              // no rate -> 1 MHz
    CHECK(load_signal(p, 16, "x.iq", 0.0).sample_rate == kDefaultIqSampleRate);         // 0 counts as absent (`or`)
    CHECK(load_signal(p, 16, "dir.v2/x.IQ").num_samples == 2);
    CHECK_THROWS(load_signal(p, 16, "x.iq", -5.0));                                     // negative is passed through and rejected
    CHECK_THROWS(load_signal(p, 16, "x.bin"));
    CHECK_THROWS(load_signal(p, 16, "noext"));
    CHECK_THROWS(load_signal(p, 16, ".wav"));                                           // a dotfile has no suffix
    CHECK_THROWS(load_signal(p, 15, "x.iq"));                                           // misaligned
    std::string msg;
    try { load_signal(p, 16, "x.bin"); } catch (const std::exception& e) { msg = e.what(); }
    CHECK(msg == "unsupported file extension: '.bin' (expected .wav or .iq)");
}

// The native app's own dispatcher tests (test_fec_dispatcher.py, test_interleave_dispatcher.py).
TEST_CASE("native: fec dispatcher round-trips each scheme in its own unit (bits vs bytes)") {
    const auto bits = random_bits(64, 0);
    for (const char* scheme : {"viterbi", "ldpc"}) {
        CHECK(fec_decode(fec_encode(bits, scheme), scheme, 64) == bits);              // msg_len in bits
    }
    const auto byte_bits = random_bits(64, 1);
    for (const char* scheme : {"rs", "concatenated"}) {
        CHECK(fec_decode(fec_encode(byte_bits, scheme), scheme, 64 / 8) == byte_bits);   // msg_len in bytes
    }
    CHECK_THROWS(fec_encode(random_bits(8), "not_a_scheme"));
    CHECK_THROWS(fec_decode(random_bits(8), "not_a_scheme", 8));
}

TEST_CASE("native: rs pads bits that are not a whole number of bytes with zeros") {
    const auto bits = random_bits(20, 2);                          // not a multiple of 8
    const auto decoded = fec_decode(fec_encode(bits, "rs"), "rs", 3);   // ceil(20 / 8) = 3 bytes -> 24 bits back
    CHECK(decoded.size() == 24);
    CHECK(std::equal(bits.begin(), bits.end(), decoded.begin()));
    for (size_t i = 20; i < 24; ++i) CHECK(decoded[i] == 0);
}

TEST_CASE("native: interleave dispatcher round-trips every kind; convolutional after its delay offset") {
    const auto bits = random_bits(24, 0);
    for (const auto& spec : {InterleaverSpec::block(4, 6), InterleaverSpec::diagonal(4, 6), InterleaverSpec::pseudo_random(7)}) {
        CHECK(deinterleave(interleave(bits, spec), spec) == bits);
    }
    const auto conv = InterleaverSpec::convolutional(3, 2);
    const auto recovered = deinterleave(interleave(bits, conv), conv);
    const size_t offset = 3 * 2 * 2;                               // branches * (branches - 1) * delay
    CHECK(std::equal(recovered.begin() + static_cast<long>(offset), recovered.end(), bits.begin()));
    CHECK_THROWS(interleave(random_bits(8), InterleaverSpec{"not_a_scheme", {}, {}, {}, {}, {}}));
    CHECK_THROWS(deinterleave(random_bits(8), InterleaverSpec{"not_a_scheme", {}, {}, {}, {}, {}}));
}

TEST_CASE("native: the interleave dispatcher is case-insensitive") {
    const auto bits = random_bits(24, 0);
    CHECK(interleave(bits, InterleaverSpec{"BLOCK", 4u, 6u, {}, {}, {}}) == interleave(bits, InterleaverSpec::block(4, 6)));
    CHECK(fec_encode(bits, "LDPC") == fec_encode(bits, "ldpc"));
}

TEST_MAIN()
