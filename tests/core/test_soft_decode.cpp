// Feature 3 (SIH26147 extra feature): soft-decision decoding chain -- LLR demapping,
// LLR-carrying interleavers, and soft Viterbi/LDPC. Hard decision from the same LLRs
// must always agree with the ordinary hard demodulator/decoder on a clean signal (the
// two paths differ only in the information carried between stages, never in the
// underlying answer when there is nothing to disagree about); the real point of "soft"
// is that it should do at least as well, and often much better, once there is noise.

#include <cmath>
#include <complex>
#include <vector>

#include "demod.hpp"
#include "fec.hpp"
#include "interleave.hpp"
#include "soft_demod.hpp"
#include "viterbi.hpp"
#include "../harness.hpp"
#include "../testutil.hpp"

using namespace sigcore;
using namespace sigtest;
using cf = std::complex<float>;

namespace {

std::vector<cf> add_noise(std::vector<cf> s, float amplitude, uint64_t seed) {
    Rng rng(seed);
    for (auto& v : s) v += cf(amplitude * rng.uniform(), amplitude * rng.uniform());
    return s;
}

unsigned bit_errors(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    unsigned n = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) n += (a[i] != b[i]);
    return n;
}

}  // namespace

// --- soft demapping ---------------------------------------------------------------

TEST_CASE("soft demod: hard decision from the LLRs matches the ordinary hard demodulator, noiseless") {
    const auto bits = random_bits(240, 1);
    for (auto scheme : {ModulationScheme::BPSK, ModulationScheme::QPSK, ModulationScheme::PSK8,
                        ModulationScheme::QAM16, ModulationScheme::QAM64, ModulationScheme::ASK4}) {
        const auto samples = linear_modulate(bits, scheme);
        const float sigma2 = estimate_noise_variance_linear(samples, scheme);
        CHECK(sigma2 > 0.0f);
        const auto llrs = soft_demodulate_linear(samples, scheme, sigma2);
        CHECK(llrs.size() == bits.size());
        const auto hard = llr_hard_decision(llrs);
        const auto reference = linear_demodulate(samples, scheme);
        CHECK(hard == std::vector<uint8_t>(reference.begin(), reference.begin() + static_cast<long>(bits.size())));
    }
}

TEST_CASE("soft demod: LLR sign matches the transmitted bit, and magnitude grows with SNR") {
    const auto bits = random_bits(480, 2);
    for (float amplitude : {0.5f, 0.1f}) {
        const auto samples = add_noise(linear_modulate(bits, ModulationScheme::QPSK), amplitude, 3);
        const float sigma2 = estimate_noise_variance_linear(samples, ModulationScheme::QPSK);
        const auto llrs = soft_demodulate_linear(samples, ModulationScheme::QPSK, sigma2);
        unsigned agree = 0;
        double mean_abs = 0;
        for (size_t i = 0; i < bits.size(); ++i) {
            agree += ((llrs[i] < 0) == (bits[i] == 1));
            mean_abs += std::fabs(llrs[i]);
        }
        CHECK(agree > bits.size() * 9 / 10);  // well above chance at these noise levels
        mean_abs /= bits.size();
        if (amplitude == 0.5f) {
            static double high_noise_mean_abs = 0;
            high_noise_mean_abs = mean_abs;
            (void)high_noise_mean_abs;
        }
    }
    // Directly compare mean |LLR| at two noise levels on the same data.
    const auto clean = linear_modulate(bits, ModulationScheme::QPSK);
    const auto noisy_low = add_noise(clean, 0.05f, 4);
    const auto noisy_high = add_noise(clean, 0.4f, 4);
    const float s2_low = estimate_noise_variance_linear(noisy_low, ModulationScheme::QPSK);
    const float s2_high = estimate_noise_variance_linear(noisy_high, ModulationScheme::QPSK);
    const auto llr_low = soft_demodulate_linear(noisy_low, ModulationScheme::QPSK, s2_low);
    const auto llr_high = soft_demodulate_linear(noisy_high, ModulationScheme::QPSK, s2_high);
    auto mean_abs_of = [](const LLRBuffer& v) {
        double m = 0;
        for (float x : v) m += std::fabs(x);
        return m / v.size();
    };
    CHECK(mean_abs_of(llr_low) > mean_abs_of(llr_high));  // cleaner signal -> more confident LLRs
}

// --- LLR-carrying interleavers ------------------------------------------------------

TEST_CASE("interleavers move LLRs through exactly the same permutation as bits") {
    const auto bits = random_bits(48, 6);
    LLRBuffer llrs(bits.size());
    for (size_t i = 0; i < bits.size(); ++i) llrs[i] = bits[i] ? -3.0f : 3.0f;  // llr<0 <=> bit=1

    auto check_matches = [&](const std::vector<uint8_t>& bit_result, const LLRBuffer& llr_result) {
        CHECK(bit_result.size() == llr_result.size());
        for (size_t i = 0; i < bit_result.size(); ++i) CHECK((llr_result[i] < 0) == (bit_result[i] == 1));
    };
    check_matches(block_interleave(bits, 6, 8), block_interleave(llrs, 6, 8));
    check_matches(block_deinterleave(bits, 6, 8), block_deinterleave(llrs, 6, 8));
    check_matches(diagonal_interleave(bits, 6, 8), diagonal_interleave(llrs, 6, 8));
    check_matches(pseudo_random_interleave(bits, 99), pseudo_random_interleave(llrs, 99));
    check_matches(convolutional_interleave(bits, 4, 2), convolutional_interleave(llrs, 4, 2));
    // round trip on floats, same offset semantics as the bit version
    const auto conv_il = convolutional_interleave(llrs, 3, 2);
    const auto conv_dil = convolutional_deinterleave(conv_il, 3, 2);
    const size_t offset = 3 * 2 * 2;
    for (size_t i = offset; i < llrs.size(); ++i) CHECK(conv_dil[i] == llrs[i - offset]);
}

// --- soft Viterbi ------------------------------------------------------------------

TEST_CASE("soft viterbi: noiseless round trip, and matches hard decoding of the same LLRs' signs") {
    const auto msg = random_bits(96, 7);
    const auto coded = viterbi_encode(msg);
    LLRBuffer llrs(coded.size());
    for (size_t i = 0; i < coded.size(); ++i) llrs[i] = coded[i] ? -5.0f : 5.0f;
    CHECK(viterbi_decode_soft(llrs, msg.size()) == msg);
    CHECK_THROWS(viterbi_decode_soft({}, 1));
    CHECK_THROWS(viterbi_decode_soft(LLRBuffer(15, 1.0f), 5));  // odd length
}

TEST_CASE("soft viterbi: bit error rate is never worse than hard decoding, across an SNR sweep") {
    // The doc's own acceptance test: soft BER <= hard BER at every point, soft decodes
    // successfully at a lower SNR than hard. Measured, not assumed.
    const auto msg = random_bits(400, 8);
    const auto coded = viterbi_encode(msg);
    const auto symbols = linear_modulate(coded, ModulationScheme::BPSK);

    for (float amplitude : {0.05f, 0.3f, 0.5f, 0.7f, 0.9f}) {
        const auto noisy = add_noise(symbols, amplitude, 42);
        const auto hard_bits = linear_demodulate(noisy, ModulationScheme::BPSK);
        const auto hard_decoded = viterbi_decode(std::vector<uint8_t>(hard_bits.begin(), hard_bits.begin() + static_cast<long>(coded.size())), msg.size());

        const float sigma2 = estimate_noise_variance_linear(noisy, ModulationScheme::BPSK);
        const auto llrs = soft_demodulate_linear(noisy, ModulationScheme::BPSK, sigma2);
        const auto soft_decoded = viterbi_decode_soft(llrs, msg.size());

        const unsigned hard_err = bit_errors(hard_decoded, msg);
        const unsigned soft_err = bit_errors(soft_decoded, msg);
        CHECK(soft_err <= hard_err);
    }
}

// --- soft LDPC ----------------------------------------------------------------------

TEST_CASE("soft ldpc: noiseless round trip") {
    const auto msg = random_bits(64, 9);
    const auto coded = ldpc_encode(msg);
    LLRBuffer llrs(coded.size());
    for (size_t i = 0; i < coded.size(); ++i) llrs[i] = coded[i] ? -5.0f : 5.0f;
    CHECK(ldpc_decode_soft(llrs, msg.size()) == msg);
    CHECK_THROWS(ldpc_decode_soft({}, 1));
    CHECK_THROWS(ldpc_decode_soft(LLRBuffer(15, 1.0f), 8));  // not a multiple of 16
}

TEST_CASE("soft ldpc: corrects a weak, wrongly-signed channel LLR via message passing from its neighbours") {
    // Every channel LLR is strong and correct except one, which is weak AND has the
    // wrong sign (as if that one bit's demapper were unsure and leaning the wrong way).
    // Only propagation through the parity checks (the check-to-variable step) can fix
    // this -- the channel value alone points the wrong way.
    const auto msg = random_bits(8, 11);
    const auto coded = ldpc_encode(msg);
    for (size_t flip_pos = 0; flip_pos < coded.size(); ++flip_pos) {
        LLRBuffer llrs(coded.size());
        for (size_t i = 0; i < coded.size(); ++i) llrs[i] = coded[i] ? -6.0f : 6.0f;
        llrs[flip_pos] = coded[flip_pos] ? 0.3f : -0.3f;  // weak, wrong sign
        CHECK(ldpc_decode_soft(llrs, msg.size()) == msg);
    }
}

TEST_CASE("soft ldpc: bit error rate is never worse than hard decoding, across an SNR sweep") {
    const auto msg = random_bits(80, 10);
    const auto coded = ldpc_encode(msg);
    const auto symbols = linear_modulate(coded, ModulationScheme::BPSK);

    for (float amplitude : {0.05f, 0.3f, 0.5f, 0.7f}) {
        const auto noisy = add_noise(symbols, amplitude, 55);
        const auto hard_bits = linear_demodulate(noisy, ModulationScheme::BPSK);
        const auto hard_decoded = ldpc_decode(std::vector<uint8_t>(hard_bits.begin(), hard_bits.begin() + static_cast<long>(coded.size())), msg.size());

        const float sigma2 = estimate_noise_variance_linear(noisy, ModulationScheme::BPSK);
        const auto llrs = soft_demodulate_linear(noisy, ModulationScheme::BPSK, sigma2);
        const auto soft_decoded = ldpc_decode_soft(llrs, msg.size());

        CHECK(bit_errors(soft_decoded, msg) <= bit_errors(hard_decoded, msg));
    }
}

TEST_MAIN()
