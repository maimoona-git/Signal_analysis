#include "wav_io.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace sigcore {

namespace {

uint32_t read_u32le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t read_u16le(const uint8_t* p) {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
}

constexpr uint16_t WAVE_FORMAT_PCM = 1;
constexpr uint16_t WAVE_FORMAT_IEEE_FLOAT = 3;

}  // namespace

SignalData parse_wav(const uint8_t* data, size_t size, const std::string& source) {
    if (size < 12 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0) {
        throw std::runtime_error("wav: not a valid RIFF/WAVE file: " + source);
    }

    uint16_t audio_format = 0;
    uint16_t num_channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits_per_sample = 0;
    bool have_fmt = false;

    size_t data_offset = 0;
    size_t data_size = 0;
    bool data_truncated = false;
    bool have_data = false;

    size_t pos = 12;
    while (pos + 8 <= size) {
        const uint8_t* chunk_id = data + pos;
        const size_t chunk_size = read_u32le(data + pos + 4);
        pos += 8;
        const size_t available = size - pos;

        if (std::memcmp(chunk_id, "fmt ", 4) == 0) {
            if (chunk_size > available) {
                throw std::runtime_error("wav: truncated fmt chunk: " + source);
            }
            if (chunk_size < 16) {
                throw std::runtime_error("wav: fmt chunk too small: " + source);
            }
            audio_format = read_u16le(data + pos + 0);
            num_channels = read_u16le(data + pos + 2);
            sample_rate = read_u32le(data + pos + 4);
            bits_per_sample = read_u16le(data + pos + 14);
            have_fmt = true;
        } else if (std::memcmp(chunk_id, "data", 4) == 0) {
            data_offset = pos;
            data_size = std::min(chunk_size, available);
            data_truncated = chunk_size > available;
            have_data = true;
        }

        // Advance past the chunk (plus its pad byte); a chunk running off the
        // end of the buffer means the file is truncated, so stop scanning.
        const size_t advance = chunk_size + (chunk_size & 1u);
        if (advance > available) {
            break;
        }
        pos += advance;
    }

    if (!have_fmt || !have_data) {
        throw std::runtime_error("wav: missing fmt or data chunk: " + source);
    }
    if (num_channels != 1 && num_channels != 2) {
        throw std::runtime_error("wav: only mono or stereo files are supported: " + source);
    }
    if (audio_format != WAVE_FORMAT_PCM && audio_format != WAVE_FORMAT_IEEE_FLOAT) {
        throw std::runtime_error("wav: unsupported audio format code: " + source);
    }

    const size_t bytes_per_sample = bits_per_sample / 8;
    const size_t frame_size = bytes_per_sample * num_channels;
    if (frame_size == 0) {
        throw std::runtime_error("wav: data size not aligned to frame size: " + source);
    }
    if (data_truncated) {
        data_size -= data_size % frame_size;  // drop a trailing partial frame
    } else if (data_size % frame_size != 0) {
        throw std::runtime_error("wav: data size not aligned to frame size: " + source);
    }
    const size_t num_frames = data_size / frame_size;

    auto read_sample = [&](const uint8_t* p) -> float {
        if (audio_format == WAVE_FORMAT_IEEE_FLOAT && bits_per_sample == 32) {
            float v;
            std::memcpy(&v, p, sizeof(float));
            return v;
        } else if (audio_format == WAVE_FORMAT_PCM && bits_per_sample == 16) {
            int16_t v = static_cast<int16_t>(read_u16le(p));
            return static_cast<float>(v) / 32768.0f;
        } else if (audio_format == WAVE_FORMAT_PCM && bits_per_sample == 8) {
            return (static_cast<float>(p[0]) - 128.0f) / 128.0f;
        } else {
            throw std::runtime_error("wav: unsupported bit depth: " + source);
        }
    };

    SignalData sig;
    sig.sample_rate = static_cast<double>(sample_rate);
    sig.format = num_channels == 1 ? "wav_mono_real" : "wav_stereo_iq";
    sig.samples.resize(num_frames);

    for (size_t i = 0; i < num_frames; ++i) {
        const uint8_t* frame_ptr = data + data_offset + i * frame_size;
        if (num_channels == 1) {
            sig.samples[i] = std::complex<float>(read_sample(frame_ptr), 0.0f);
        } else {
            float re = read_sample(frame_ptr);
            float im = read_sample(frame_ptr + bytes_per_sample);
            sig.samples[i] = std::complex<float>(re, im);
        }
    }

    return sig;
}

SignalData load_wav(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        throw std::runtime_error("wav: could not open file: " + path);
    }
    std::streamsize size = f.tellg();
    f.seekg(0, std::ios::beg);
    if (size < 0) {
        throw std::runtime_error("wav: could not determine file size: " + path);
    }
    std::vector<uint8_t> raw(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(raw.data()), size);
    if (static_cast<std::streamsize>(f.gcount()) != size) {
        throw std::runtime_error("wav: failed to read full file: " + path);
    }
    return parse_wav(raw.data(), raw.size(), path);
}

}  // namespace sigcore
