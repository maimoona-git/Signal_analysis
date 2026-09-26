#include <pybind11/complex.h>
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstring>

#include "demod.hpp"
#include "dsp.hpp"
#include "fec.hpp"
#include "interleave.hpp"
#include "iq_io.hpp"
#include "signal_data.hpp"
#include "wav_io.hpp"

namespace py = pybind11;

namespace {

py::dict signal_data_to_dict(const sigcore::SignalData& sig) {
    py::array_t<std::complex<float>, py::array::c_style | py::array::forcecast> samples(sig.samples.size());
    std::memcpy(samples.mutable_data(), sig.samples.data(),
                sig.samples.size() * sizeof(std::complex<float>));

    py::dict result;
    result["samples"] = samples;
    result["sample_rate"] = sig.sample_rate;
    result["format"] = sig.format;
    result["num_samples"] = sig.num_samples();
    result["duration_sec"] = sig.duration_sec();
    return result;
}

std::vector<std::complex<float>> array_to_vector(const py::array_t<std::complex<float>, py::array::c_style | py::array::forcecast>& arr) {
    auto buf = arr.request();
    if (buf.ndim != 1) {
        throw std::runtime_error("expected a 1-D complex64 array");
    }
    const auto* ptr = static_cast<const std::complex<float>*>(buf.ptr);
    return std::vector<std::complex<float>>(ptr, ptr + buf.shape[0]);
}

py::array_t<std::complex<float>, py::array::c_style | py::array::forcecast> vector_to_array(const std::vector<std::complex<float>>& v) {
    py::array_t<std::complex<float>, py::array::c_style | py::array::forcecast> arr(v.size());
    std::memcpy(arr.mutable_data(), v.data(), v.size() * sizeof(std::complex<float>));
    return arr;
}

std::vector<uint8_t> array_to_bits(const py::array_t<uint8_t, py::array::c_style | py::array::forcecast>& arr) {
    auto buf = arr.request();
    if (buf.ndim != 1) {
        throw std::runtime_error("expected a 1-D uint8 array");
    }
    const auto* ptr = static_cast<const uint8_t*>(buf.ptr);
    return std::vector<uint8_t>(ptr, ptr + buf.shape[0]);
}

py::array_t<uint8_t, py::array::c_style | py::array::forcecast> bits_to_array(const std::vector<uint8_t>& v) {
    py::array_t<uint8_t, py::array::c_style | py::array::forcecast> arr(v.size());
    std::memcpy(arr.mutable_data(), v.data(), v.size() * sizeof(uint8_t));
    return arr;
}

}  // namespace

PYBIND11_MODULE(sigcore, m) {
    m.doc() = "sigcore: C++ DSP/FEC engine for signal analysis (PS 26147)";
    m.attr("__version__") = "0.1.0";

    m.def(
        "load_wav",
        [](const std::string& path) { return signal_data_to_dict(sigcore::load_wav(path)); },
        py::arg("path"),
        "Load a .wav file. Returns a dict with keys: samples (complex64 ndarray), "
        "sample_rate, format, num_samples, duration_sec.");

    m.def(
        "load_iq",
        [](const std::string& path, double sample_rate) {
            return signal_data_to_dict(sigcore::load_iq(path, sample_rate));
        },
        py::arg("path"), py::arg("sample_rate"),
        "Load a raw .IQ file (interleaved float32 I/Q, no header). Returns a dict "
        "with keys: samples (complex64 ndarray), sample_rate, format, num_samples, "
        "duration_sec.");

    m.def(
        "fft",
        [](const py::array_t<std::complex<float>, py::array::c_style | py::array::forcecast>& x) {
            return vector_to_array(sigcore::fft_forward(array_to_vector(x)));
        },
        py::arg("x"), "Forward FFT of a complex64 signal (not fft-shifted).");

    m.def(
        "compute_psd",
        [](const py::array_t<std::complex<float>, py::array::c_style | py::array::forcecast>& x, unsigned int nfft) {
            auto psd = sigcore::compute_psd(array_to_vector(x), nfft);
            py::array_t<float> arr(psd.size());
            std::memcpy(arr.mutable_data(), psd.data(), psd.size() * sizeof(float));
            return arr;
        },
        py::arg("x"), py::arg("nfft"),
        "Power spectral density estimate (dB, fft-shifted), length nfft.");

    m.def(
        "compute_spectrogram",
        [](const py::array_t<std::complex<float>, py::array::c_style | py::array::forcecast>& x, unsigned int nfft, unsigned int hop) {
            unsigned int num_frames = 0;
            auto flat = sigcore::compute_spectrogram(array_to_vector(x), nfft, hop, &num_frames);

            py::array_t<float> arr({static_cast<py::ssize_t>(num_frames), static_cast<py::ssize_t>(nfft)});
            std::memcpy(arr.mutable_data(), flat.data(), flat.size() * sizeof(float));
            return arr;
        },
        py::arg("x"), py::arg("nfft"), py::arg("hop"),
        "Short-time Fourier transform magnitude spectrogram (dB, fft-shifted per "
        "row). Returns a 2D array of shape (num_frames, nfft).");

    m.def(
        "lowpass_filter",
        [](const py::array_t<std::complex<float>, py::array::c_style | py::array::forcecast>& x, unsigned int n_taps, float fc, float as_db) {
            return vector_to_array(sigcore::lowpass_filter(array_to_vector(x), n_taps, fc, as_db));
        },
        py::arg("x"), py::arg("n_taps"), py::arg("fc"), py::arg("as_db") = 60.0f,
        "Lowpass-filter a complex64 signal with a Kaiser-windowed FIR filter. "
        "fc is normalized cutoff frequency (0 < fc < 0.5).");

    py::enum_<sigcore::ModulationScheme>(m, "ModulationScheme")
        .value("BPSK", sigcore::ModulationScheme::BPSK)
        .value("QPSK", sigcore::ModulationScheme::QPSK)
        .value("PSK8", sigcore::ModulationScheme::PSK8)
        .value("PSK16", sigcore::ModulationScheme::PSK16)
        .value("PSK32", sigcore::ModulationScheme::PSK32)
        .value("PSK64", sigcore::ModulationScheme::PSK64)
        .value("QAM4", sigcore::ModulationScheme::QAM4)
        .value("QAM16", sigcore::ModulationScheme::QAM16)
        .value("QAM32", sigcore::ModulationScheme::QAM32)
        .value("QAM64", sigcore::ModulationScheme::QAM64)
        .value("QAM128", sigcore::ModulationScheme::QAM128)
        .value("QAM256", sigcore::ModulationScheme::QAM256)
        .value("ASK2", sigcore::ModulationScheme::ASK2)
        .value("ASK4", sigcore::ModulationScheme::ASK4)
        .value("ASK8", sigcore::ModulationScheme::ASK8)
        .value("ASK16", sigcore::ModulationScheme::ASK16)
        .value("OOK", sigcore::ModulationScheme::OOK);

    m.def(
        "linear_modulate",
        [](const py::array_t<uint8_t, py::array::c_style | py::array::forcecast>& bits, sigcore::ModulationScheme scheme) {
            return vector_to_array(sigcore::linear_modulate(array_to_bits(bits), scheme));
        },
        py::arg("bits"), py::arg("scheme"),
        "Modulate a bitstream (uint8, one bit per element) into complex64 symbols "
        "using a linear (PSK/QAM/ASK/OOK) scheme.");

    m.def(
        "linear_demodulate",
        [](const py::array_t<std::complex<float>, py::array::c_style | py::array::forcecast>& samples, sigcore::ModulationScheme scheme) {
            return bits_to_array(sigcore::linear_demodulate(array_to_vector(samples), scheme));
        },
        py::arg("samples"), py::arg("scheme"),
        "Hard-decision demodulate complex64 symbols into a bitstream (uint8, one "
        "bit per element) using a linear (PSK/QAM/ASK/OOK) scheme.");

    m.def(
        "fsk_modulate",
        [](const py::array_t<uint8_t, py::array::c_style | py::array::forcecast>& bits, unsigned int bps, unsigned int sps, float bandwidth) {
            return vector_to_array(sigcore::fsk_modulate(array_to_bits(bits), bps, sps, bandwidth));
        },
        py::arg("bits"), py::arg("bps"), py::arg("sps"), py::arg("bandwidth"),
        "Modulate a bitstream into complex64 samples using M-ary FSK.");

    m.def(
        "fsk_demodulate",
        [](const py::array_t<std::complex<float>, py::array::c_style | py::array::forcecast>& samples, unsigned int bps, unsigned int sps,
           float bandwidth) {
            return bits_to_array(sigcore::fsk_demodulate(array_to_vector(samples), bps, sps, bandwidth));
        },
        py::arg("samples"), py::arg("bps"), py::arg("sps"), py::arg("bandwidth"),
        "Demodulate M-ary FSK complex64 samples into a bitstream, assuming perfect "
        "symbol timing.");

    // --- Interleavers (operate on unpacked bits: uint8 array, one bit per element) ---

    m.def(
        "block_interleave",
        [](const py::array_t<uint8_t, py::array::c_style | py::array::forcecast>& bits, unsigned int rows, unsigned int cols) {
            return bits_to_array(sigcore::block_interleave(array_to_bits(bits), rows, cols));
        },
        py::arg("bits"), py::arg("rows"), py::arg("cols"));
    m.def(
        "block_deinterleave",
        [](const py::array_t<uint8_t, py::array::c_style | py::array::forcecast>& bits, unsigned int rows, unsigned int cols) {
            return bits_to_array(sigcore::block_deinterleave(array_to_bits(bits), rows, cols));
        },
        py::arg("bits"), py::arg("rows"), py::arg("cols"));

    m.def(
        "diagonal_interleave",
        [](const py::array_t<uint8_t, py::array::c_style | py::array::forcecast>& bits, unsigned int rows, unsigned int cols) {
            return bits_to_array(sigcore::diagonal_interleave(array_to_bits(bits), rows, cols));
        },
        py::arg("bits"), py::arg("rows"), py::arg("cols"));
    m.def(
        "diagonal_deinterleave",
        [](const py::array_t<uint8_t, py::array::c_style | py::array::forcecast>& bits, unsigned int rows, unsigned int cols) {
            return bits_to_array(sigcore::diagonal_deinterleave(array_to_bits(bits), rows, cols));
        },
        py::arg("bits"), py::arg("rows"), py::arg("cols"));

    m.def(
        "pseudo_random_interleave",
        [](const py::array_t<uint8_t, py::array::c_style | py::array::forcecast>& bits, unsigned int seed) {
            return bits_to_array(sigcore::pseudo_random_interleave(array_to_bits(bits), seed));
        },
        py::arg("bits"), py::arg("seed"));
    m.def(
        "pseudo_random_deinterleave",
        [](const py::array_t<uint8_t, py::array::c_style | py::array::forcecast>& bits, unsigned int seed) {
            return bits_to_array(sigcore::pseudo_random_deinterleave(array_to_bits(bits), seed));
        },
        py::arg("bits"), py::arg("seed"));

    m.def(
        "convolutional_interleave",
        [](const py::array_t<uint8_t, py::array::c_style | py::array::forcecast>& bits, unsigned int branches, unsigned int delay_increment) {
            return bits_to_array(
                sigcore::convolutional_interleave(array_to_bits(bits), branches, delay_increment));
        },
        py::arg("bits"), py::arg("branches"), py::arg("delay_increment"));
    m.def(
        "convolutional_deinterleave",
        [](const py::array_t<uint8_t, py::array::c_style | py::array::forcecast>& bits, unsigned int branches, unsigned int delay_increment) {
            return bits_to_array(sigcore::convolutional_deinterleave(array_to_bits(bits), branches,
                                                                       delay_increment));
        },
        py::arg("bits"), py::arg("branches"), py::arg("delay_increment"));

    // --- FEC (Viterbi/Reed-Solomon/Concatenated operate on byte messages; LDPC on unpacked bits) ---

    m.def(
        "viterbi_encode",
        [](const py::array_t<uint8_t, py::array::c_style | py::array::forcecast>& msg) { return bits_to_array(sigcore::viterbi_encode(array_to_bits(msg))); },
        py::arg("msg"));
    m.def(
        "viterbi_decode",
        [](const py::array_t<uint8_t, py::array::c_style | py::array::forcecast>& encoded, size_t msg_len) {
            return bits_to_array(sigcore::viterbi_decode(array_to_bits(encoded), msg_len));
        },
        py::arg("encoded"), py::arg("msg_len"));

    m.def(
        "rs_encode",
        [](const py::array_t<uint8_t, py::array::c_style | py::array::forcecast>& msg) { return bits_to_array(sigcore::rs_encode(array_to_bits(msg))); },
        py::arg("msg"));
    m.def(
        "rs_decode",
        [](const py::array_t<uint8_t, py::array::c_style | py::array::forcecast>& encoded, size_t msg_len) {
            return bits_to_array(sigcore::rs_decode(array_to_bits(encoded), msg_len));
        },
        py::arg("encoded"), py::arg("msg_len"));

    m.def(
        "concatenated_encode",
        [](const py::array_t<uint8_t, py::array::c_style | py::array::forcecast>& msg) {
            return bits_to_array(sigcore::concatenated_encode(array_to_bits(msg)));
        },
        py::arg("msg"));
    m.def(
        "concatenated_decode",
        [](const py::array_t<uint8_t, py::array::c_style | py::array::forcecast>& encoded, size_t msg_len) {
            return bits_to_array(sigcore::concatenated_decode(array_to_bits(encoded), msg_len));
        },
        py::arg("encoded"), py::arg("msg_len"));

    m.def(
        "ldpc_encode",
        [](const py::array_t<uint8_t, py::array::c_style | py::array::forcecast>& msg_bits) {
            return bits_to_array(sigcore::ldpc_encode(array_to_bits(msg_bits)));
        },
        py::arg("msg_bits"));
    m.def(
        "ldpc_decode",
        [](const py::array_t<uint8_t, py::array::c_style | py::array::forcecast>& encoded_bits, size_t msg_len_bits) {
            return bits_to_array(sigcore::ldpc_decode(array_to_bits(encoded_bits), msg_len_bits));
        },
        py::arg("encoded_bits"), py::arg("msg_len_bits"));
}
