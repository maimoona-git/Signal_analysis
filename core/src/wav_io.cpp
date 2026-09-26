#include "wav_io.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace sigcore {

namespace {

uint32_t read_u32le(const char* p) {
    return static_cast<uint32_t>(static_cast<unsigned char>(p[0])) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[3])) << 24);
}

uint16_t read_u16le(const char* p) {
    return static_cast<uint16_t>(static_cast<unsigned char>(p[0])) |
           (static_cast<uint16_t>(static_cast<unsigned char>(p[1])) << 8);
}

constexpr uint16_t WAVE_FORMAT_PCM = 1;
constexpr uint16_t WAVE_FORMAT_IEEE_FLOAT = 3;

}  // namespace

SignalData load_wav(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        throw std::runtime_error("wav: could not open file: " + path);
    }

    std::vector<char> header(12);
    f.read(header.data(), 12);
    if (f.gcount() != 12 || std::strncmp(header.data(), "RIFF", 4) != 0 ||
        std::strncmp(header.data() + 8, "WAVE", 4) != 0) {
        throw std::runtime_error("wav: not a valid RIFF/WAVE file: " + path);
    }

    uint16_t audio_format = 0;
    uint16_t num_channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits_per_sample = 0;
    bool have_fmt = false;

    std::vector<char> pcm_data;
    bool have_data = false;

    while (f) {
        char chunk_id[4];
        char chunk_size_buf[4];
        f.read(chunk_id, 4);
        if (f.gcount() != 4) break;
        f.read(chunk_size_buf, 4);
        if (f.gcount() != 4) break;
        uint32_t chunk_size = read_u32le(chunk_size_buf);

        if (std::strncmp(chunk_id, "fmt ", 4) == 0) {
            std::vector<char> fmt(chunk_size);
            f.read(fmt.data(), chunk_size);
            if (fmt.size() < 16) {
                throw std::runtime_error("wav: fmt chunk too small: " + path);
            }
            audio_format = read_u16le(fmt.data() + 0);
            num_channels = read_u16le(fmt.data() + 2);
            sample_rate = read_u32le(fmt.data() + 4);
            bits_per_sample = read_u16le(fmt.data() + 14);
            have_fmt = true;
        } else if (std::strncmp(chunk_id, "data", 4) == 0) {
            pcm_data.resize(chunk_size);
            f.read(pcm_data.data(), chunk_size);
            have_data = true;
        } else {
            f.seekg(chunk_size, std::ios::cur);
        }

        if (chunk_size % 2 == 1) {
            f.seekg(1, std::ios::cur);
        }
    }

    if (!have_fmt || !have_data) {
        throw std::runtime_error("wav: missing fmt or data chunk: " + path);
    }
    if (num_channels != 1 && num_channels != 2) {
        throw std::runtime_error("wav: only mono or stereo files are supported: " + path);
    }
    if (audio_format != WAVE_FORMAT_PCM && audio_format != WAVE_FORMAT_IEEE_FLOAT) {
        throw std::runtime_error("wav: unsupported audio format code: " + path);
    }

    const size_t bytes_per_sample = bits_per_sample / 8;
    const size_t frame_size = bytes_per_sample * num_channels;
    if (frame_size == 0 || pcm_data.size() % frame_size != 0) {
        throw std::runtime_error("wav: data size not aligned to frame size: " + path);
    }
    const size_t num_frames = pcm_data.size() / frame_size;

    auto read_sample = [&](const char* p) -> float {
        if (audio_format == WAVE_FORMAT_IEEE_FLOAT && bits_per_sample == 32) {
            float v;
            std::memcpy(&v, p, sizeof(float));
            return v;
        } else if (audio_format == WAVE_FORMAT_PCM && bits_per_sample == 16) {
            int16_t v = static_cast<int16_t>(read_u16le(p));
            return static_cast<float>(v) / 32768.0f;
        } else if (audio_format == WAVE_FORMAT_PCM && bits_per_sample == 8) {
            uint8_t v = static_cast<uint8_t>(p[0]);
            return (static_cast<float>(v) - 128.0f) / 128.0f;
        } else {
            throw std::runtime_error("wav: unsupported bit depth: " + path);
        }
    };

    SignalData sig;
    sig.sample_rate = static_cast<double>(sample_rate);
    sig.samples.resize(num_frames);

    for (size_t i = 0; i < num_frames; ++i) {
        const char* frame_ptr = pcm_data.data() + i * frame_size;
        if (num_channels == 1) {
            float re = read_sample(frame_ptr);
            sig.samples[i] = std::complex<float>(re, 0.0f);
            sig.format = "wav_mono_real";
        } else {
            float re = read_sample(frame_ptr);
            float im = read_sample(frame_ptr + bytes_per_sample);
            sig.samples[i] = std::complex<float>(re, im);
            sig.format = "wav_stereo_iq";
        }
    }

    return sig;
}

}  // namespace sigcore
