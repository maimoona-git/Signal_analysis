// Embind surface of the bridge (WASM only), the JS-visible half of:
//   - file bytes in:  ingestFile(bytes, filename, sampleRate) -> signal object
//   - ML call-out:    classifySpectrogram(spectrogram, rows, cols, callback)
//                     (drives ml_bridge.cpp: C++ -> Module.sigMlClassifier -> C++)
//   - both at once:   classifyCapture(bytes, filename, sampleRate, callback)
//   - everything:     runPipeline(bytes, filename, options, callback)   (pipeline_bridge.cpp)
//
// Errors cross the boundary as real JS Error objects (never raw C++ exception
// pointers): each function catches std::exception, lets its locals unwind,
// and only then throws the JS Error.

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cmath>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "ingest.hpp"
#include "ml_bridge.hpp"
#include "pipeline_bridge.hpp"

using emscripten::val;

namespace {

[[noreturn]] void throw_js_error(const std::string& message) {
    val::global("Error").new_(message).throw_();
}

// Copies an ArrayBuffer / typed array / DataView from JS into WASM memory.
// One bulk `TypedArray.set` (a memcpy), respecting byteOffset/byteLength so a
// subarray view of a larger buffer is read correctly.
std::vector<uint8_t> bytes_from_js(const val& v) {
    const val array_buffer = val::global("ArrayBuffer");
    const val uint8 = val::global("Uint8Array");
    val view = val::undefined();
    if (v.instanceof(array_buffer)) {
        view = uint8.new_(v);
    } else if (array_buffer.call<bool>("isView", v)) {
        view = uint8.new_(v["buffer"], v["byteOffset"], v["byteLength"]);
    } else {
        throw std::runtime_error("expected an ArrayBuffer or typed array of bytes");
    }
    const size_t n = view["length"].as<size_t>();
    std::vector<uint8_t> out(n);
    if (n > 0) {
        val(emscripten::typed_memory_view(n, out.data())).call<void>("set", view);
    }
    return out;
}

std::vector<float> floats_from_js(const val& v) {
    if (!v.instanceof(val::global("Float32Array"))) {
        throw std::runtime_error("expected a Float32Array");
    }
    const size_t n = v["length"].as<size_t>();
    std::vector<float> out(n);
    if (n > 0) {
        val(emscripten::typed_memory_view(n, out.data())).call<void>("set", v);
    }
    return out;
}

val signal_to_js(const sigcore::SignalData& sig) {
    val obj = val::object();
    obj.set("sampleRate", sig.sample_rate);
    obj.set("format", sig.format);
    obj.set("numSamples", static_cast<double>(sig.num_samples()));
    obj.set("durationSec", sig.duration_sec());
    // Interleaved I,Q floats. `new Float32Array(view)` copies out of WASM
    // memory, so the result stays valid after the heap grows.
    obj.set("samples", val::global("Float32Array")
                           .new_(emscripten::typed_memory_view(
                               sig.samples.size() * 2,
                               reinterpret_cast<const float*>(sig.samples.data()))));
    return obj;
}

val ingest_file(val bytes, std::string filename, double sample_rate) {
    std::string error;
    try {
        const std::vector<uint8_t> data = bytes_from_js(bytes);
        return signal_to_js(sigcore::ingest_bytes(data.data(), data.size(), filename, sample_rate));
    } catch (const std::exception& e) {
        error = e.what();
    }
    throw_js_error(error);
}

val ml_result_to_js(const sigbridge::MlResult& r) {
    val obj = val::object();
    obj.set("label", r.label);
    obj.set("confidence", r.confidence);
    obj.set("topIndex", static_cast<double>(r.top_index));
    obj.set("classes", val::array(r.classes));
    obj.set("probabilities", val::array(r.probabilities));
    return obj;
}

// callback(error, result) is invoked later, node-style: error is null on success.
void classify_spectrogram(val spectrogram, unsigned int rows, unsigned int cols, val callback) {
    std::string error;
    try {
        const std::vector<float> data = floats_from_js(spectrogram);
        sigbridge::classify_async(
            data, rows, cols,
            [callback](bool ok, const sigbridge::MlResult& result, const std::string& message) {
                if (ok) {
                    callback(val::null(), ml_result_to_js(result));
                } else {
                    callback(val::global("Error").new_(message), val::null());
                }
            });
        return;
    } catch (const std::exception& e) {
        error = e.what();
    }
    throw_js_error(error);
}

// The chain the pipeline will run, entered from JS: bytes -> samples ->
// spectrogram image (C++) -> JS classifier (onnxruntime-web) -> validated
// result back in C++ -> callback(error, result).
void classify_capture(val bytes, std::string filename, double sample_rate, val callback) {
    std::string error;
    try {
        const std::vector<uint8_t> data = bytes_from_js(bytes);
        const sigcore::SignalData signal = sigcore::ingest_bytes(data.data(), data.size(), filename, sample_rate);
        sigbridge::classify_signal(
            signal, [callback](bool ok, const sigbridge::MlResult& result, const std::string& message) {
                if (ok) {
                    callback(val::null(), ml_result_to_js(result));
                } else {
                    callback(val::global("Error").new_(message), val::null());
                }
            });
        return;
    } catch (const std::exception& e) {
        error = e.what();
    }
    throw_js_error(error);
}

// --- runPipeline: options in, result out -----------------------------------------

bool present(const val& v) { return !v.isUndefined() && !v.isNull(); }

double number_option(const val& v, const std::string& name) {
    if (v.typeOf().as<std::string>() != "number" || !std::isfinite(v.as<double>())) {
        throw std::invalid_argument("option " + name + " must be a finite number");
    }
    return v.as<double>();
}

uint32_t uint_option(const val& v, const std::string& name) {
    const double d = number_option(v, name);
    if (d < 0 || d != std::floor(d) || d > 4294967295.0) {
        throw std::invalid_argument("option " + name + " must be a non-negative integer");
    }
    return static_cast<uint32_t>(d);
}

std::string string_option(const val& v, const std::string& name) {
    if (v.typeOf().as<std::string>() != "string") throw std::invalid_argument("option " + name + " must be a string");
    return v.as<std::string>();
}

void reject_unknown_keys(const val& object, const std::set<std::string>& allowed, const std::string& where) {
    const val keys = val::global("Object").call<val>("keys", object);
    const unsigned n = keys["length"].as<unsigned>();
    for (unsigned i = 0; i < n; ++i) {
        const std::string key = keys[i].as<std::string>();
        if (!allowed.count(key)) throw std::invalid_argument("unknown " + where + " option '" + key + "'");
    }
}

// "0101", or an array / typed array of 0s and 1s.
sigapp::Bits bits_option(const val& v, const std::string& name) {
    sigapp::Bits bits;
    if (v.typeOf().as<std::string>() == "string") {
        for (char c : v.as<std::string>()) {
            if (c != '0' && c != '1') throw std::invalid_argument("option " + name + " must contain only 0 and 1");
            bits.push_back(static_cast<uint8_t>(c - '0'));
        }
        return bits;
    }
    if (!v.isArray() && !val::global("ArrayBuffer").call<bool>("isView", v)) {
        throw std::invalid_argument("option " + name + " must be a string of 0/1 or an array of bits");
    }
    const unsigned n = v["length"].as<unsigned>();
    for (unsigned i = 0; i < n; ++i) {
        const double d = number_option(v[i], name + "[" + std::to_string(i) + "]");
        if (d != 0 && d != 1) throw std::invalid_argument("option " + name + " must contain only 0 and 1");
        bits.push_back(static_cast<uint8_t>(d));
    }
    return bits;
}

sigapp::PipelineOptions options_from_js(const val& o) {
    sigapp::PipelineOptions options;
    if (!present(o)) return options;
    if (o.typeOf().as<std::string>() != "object") throw std::invalid_argument("pipeline options must be an object");
    reject_unknown_keys(o, {"sampleRate", "samplesPerSymbol", "demod", "interleaver", "fecScheme", "fecMsgLen",
                            "headerPattern", "autoDetect"}, "pipeline");

    if (present(o["sampleRate"])) options.sample_rate = number_option(o["sampleRate"], "sampleRate");
    if (present(o["samplesPerSymbol"])) options.samples_per_symbol = uint_option(o["samplesPerSymbol"], "samplesPerSymbol");
    if (present(o["fecScheme"])) options.fec_scheme = string_option(o["fecScheme"], "fecScheme");
    if (present(o["fecMsgLen"])) options.fec_msg_len = uint_option(o["fecMsgLen"], "fecMsgLen");
    if (present(o["headerPattern"])) options.header_pattern = bits_option(o["headerPattern"], "headerPattern");
    if (present(o["autoDetect"])) {
        if (o["autoDetect"].typeOf().as<std::string>() != "boolean") throw std::invalid_argument("option autoDetect must be a boolean");
        options.auto_detect = o["autoDetect"].as<bool>();
    }

    const val demod = o["demod"];
    if (present(demod)) {
        reject_unknown_keys(demod, {"bitsPerSymbol", "samplesPerSymbol", "bandwidth"}, "demod");
        if (present(demod["bitsPerSymbol"])) options.demod.bits_per_symbol = uint_option(demod["bitsPerSymbol"], "demod.bitsPerSymbol");
        if (present(demod["samplesPerSymbol"])) options.demod.samples_per_symbol = uint_option(demod["samplesPerSymbol"], "demod.samplesPerSymbol");
        if (present(demod["bandwidth"])) options.demod.bandwidth = static_cast<float>(number_option(demod["bandwidth"], "demod.bandwidth"));
    }

    const val il = o["interleaver"];
    if (present(il)) {
        reject_unknown_keys(il, {"kind", "rows", "cols", "seed", "branches", "delayIncrement"}, "interleaver");
        sigapp::InterleaverSpec spec;
        spec.kind = string_option(il["kind"], "interleaver.kind");
        if (present(il["rows"])) spec.rows = uint_option(il["rows"], "interleaver.rows");
        if (present(il["cols"])) spec.cols = uint_option(il["cols"], "interleaver.cols");
        if (present(il["seed"])) spec.seed = uint_option(il["seed"], "interleaver.seed");
        if (present(il["branches"])) spec.branches = uint_option(il["branches"], "interleaver.branches");
        if (present(il["delayIncrement"])) spec.delay_increment = uint_option(il["delayIncrement"], "interleaver.delayIncrement");
        options.interleaver = spec;
    }
    return options;
}

template <class T>
val copy_out(const char* constructor, const T* data, size_t n) {
    return val::global(constructor).new_(emscripten::typed_memory_view(n, data));  // new TypedArray(view) copies
}
val bits_out(const sigapp::Bits& b) { return copy_out("Uint8Array", b.data(), b.size()); }
val opt_bits_out(const std::optional<sigapp::Bits>& b) { return b ? bits_out(*b) : val::null(); }

val interleaver_out(const sigapp::InterleaverSpec& s) {
    val o = val::object();
    o.set("kind", s.kind);
    if (s.rows) o.set("rows", *s.rows);
    if (s.cols) o.set("cols", *s.cols);
    if (s.seed) o.set("seed", *s.seed);
    if (s.branches) o.set("branches", *s.branches);
    if (s.delay_increment) o.set("delayIncrement", *s.delay_increment);
    return o;
}

val pipeline_result_to_js(const sigapp::PipelineResult& r) {
    val out = val::object();
    out.set("filePath", r.file_path);
    out.set("sampleRate", r.sample_rate);
    out.set("numSamples", static_cast<double>(r.num_samples));
    out.set("fileFormat", r.file_format);
    out.set("samples", copy_out("Float32Array", reinterpret_cast<const float*>(r.samples.data()), r.samples.size() * 2));

    val spec = val::object();
    spec.set("data", copy_out("Float32Array", r.spectrogram.data.data(), r.spectrogram.data.size()));
    spec.set("frames", r.spectrogram.frames);
    spec.set("nfft", r.spectrogram.nfft);
    spec.set("freqs", copy_out("Float64Array", r.spectrogram.freqs.data(), r.spectrogram.freqs.size()));
    spec.set("times", copy_out("Float64Array", r.spectrogram.times.data(), r.spectrogram.times.size()));
    out.set("spectrogram", spec);

    out.set("predictedModulation", r.predicted_modulation);
    out.set("predictedConfidence", r.predicted_confidence);
    out.set("demodulatedBits", bits_out(r.demodulated_bits));
    out.set("deinterleavedBits", opt_bits_out(r.deinterleaved_bits));
    out.set("decodedBits", opt_bits_out(r.decoded_bits));

    val b = val::object();
    b.set("headerPattern", bits_out(r.boundaries.header_pattern));
    b.set("framePeriod", r.boundaries.frame_period ? val(*r.boundaries.frame_period) : val::null());
    val positions = val::array();
    for (size_t p : r.boundaries.header_positions) positions.call<void>("push", static_cast<double>(p));
    b.set("headerPositions", positions);
    val payloads = val::array();
    for (const auto& p : r.boundaries.payloads) payloads.call<void>("push", bits_out(p));
    b.set("payloads", payloads);
    out.set("boundaries", b);

    out.set("detectedInterleaver", r.detected_interleaver ? interleaver_out(*r.detected_interleaver) : val::null());
    out.set("detectedFec", r.detected_fec ? val(*r.detected_fec) : val::null());
    out.set("detectionScore", r.detection_score ? val(*r.detection_score) : val::null());
    return out;
}

// The whole pipeline, entered from JS: callback(error, result) fires once the
// ML classification has come back and the remaining stages have run.
void run_pipeline(val bytes, std::string filename, val options_js, val callback) {
    std::string error;
    try {
        const std::vector<uint8_t> data = bytes_from_js(bytes);
        const sigapp::PipelineOptions options = options_from_js(options_js);
        sigbridge::run_pipeline_async(
            data.data(), data.size(), filename, options,
            [callback](bool ok, const sigapp::PipelineResult& result, const std::string& message) {
                if (ok) {
                    callback(val::null(), pipeline_result_to_js(result));
                } else {
                    callback(val::global("Error").new_(message), val::null());
                }
            });
        return;
    } catch (const std::exception& e) {
        error = e.what();
    }
    throw_js_error(error);
}

}  // namespace

EMSCRIPTEN_BINDINGS(sig_bridge) {
    emscripten::function("ingestFile", &ingest_file);
    emscripten::function("classifySpectrogram", &classify_spectrogram);
    emscripten::function("classifyCapture", &classify_capture);
    emscripten::function("runPipeline", &run_pipeline);
}
