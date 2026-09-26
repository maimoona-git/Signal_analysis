#include "ingest.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>

#include "iq_io.hpp"
#include "wav_io.hpp"

namespace sigcore {

namespace {

bool has_wav_extension(const std::string& filename) {
    const size_t dot = filename.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = filename.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == "wav" || ext == "wave";
}

}  // namespace

FileKind detect_file_kind(const uint8_t* data, size_t size, const std::string& filename) {
    if (size >= 12 && std::memcmp(data, "RIFF", 4) == 0 && std::memcmp(data + 8, "WAVE", 4) == 0) {
        return FileKind::Wav;
    }
    return has_wav_extension(filename) ? FileKind::Wav : FileKind::Iq;
}

SignalData ingest_bytes(const uint8_t* data, size_t size, const std::string& filename,
                        double sample_rate) {
    if (detect_file_kind(data, size, filename) == FileKind::Wav) {
        return parse_wav(data, size, filename);
    }
    if (!(sample_rate > 0.0)) {
        throw std::runtime_error("a sample rate is required for raw .IQ files: " + filename);
    }
    return parse_iq(data, size, sample_rate, filename);
}

}  // namespace sigcore
