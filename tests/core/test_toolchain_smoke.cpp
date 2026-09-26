// Phase 1 smoke test: proves the dependency-free part of sigcore compiles and
// runs identically on the native and WASM targets.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

#include "fec.hpp"
#include "interleave.hpp"
#include "wav_io.hpp"
#include "../harness.hpp"

using namespace sigcore;

TEST_CASE("reed-solomon round trip corrects one byte error") {
    const std::vector<uint8_t> msg = {'S', 'I', 'H', '2', '0', '2', '6', '!'};
    auto encoded = rs_encode(msg);
    encoded[3] ^= 0x5A;
    CHECK(rs_decode(encoded, msg.size()) == msg);
}

TEST_CASE("block interleaver round trip") {
    std::vector<uint8_t> bits(8 * 16);
    for (size_t i = 0; i < bits.size(); ++i) bits[i] = (i * 7 + 3) % 5 == 0;
    CHECK(block_deinterleave(block_interleave(bits, 8, 16), 8, 16) == bits);
}

TEST_CASE("wav loader reads a 16-bit mono file through the (virtual) filesystem") {
    // Written to disk natively, to Emscripten's in-memory FS under WASM.
    const char* path = "phase1_smoke.wav";
    const int16_t samples[4] = {0, 16384, -16384, 32767};
    const uint32_t rate = 8000, data_bytes = sizeof(samples);
    {
        std::ofstream f(path, std::ios::binary);
        auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
        auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
        f.write("RIFF", 4); u32(36 + data_bytes); f.write("WAVE", 4);
        f.write("fmt ", 4); u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
        f.write("data", 4); u32(data_bytes);
        f.write(reinterpret_cast<const char*>(samples), data_bytes);
    }
    const auto sig = load_wav(path);
    std::remove(path);
    CHECK(sig.num_samples() == 4);
    CHECK_NEAR(sig.sample_rate, 8000.0, 0.0);
    CHECK_NEAR(sig.samples[1].real(), 0.5, 1e-4);
    CHECK_NEAR(sig.samples[1].imag(), 0.0, 0.0);
}

TEST_MAIN()
