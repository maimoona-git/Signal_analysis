// Embind surface of the bridge (WASM only), the JS-visible half of:
//   - file bytes in:  ingestFile(bytes, filename, sampleRate) -> signal object
//   - ML call-out:    classifySpectrogram(spectrogram, rows, cols, callback)
//                     (drives ml_bridge.cpp: C++ -> Module.sigMlClassifier -> C++)
//
// Errors cross the boundary as real JS Error objects (never raw C++ exception
// pointers): each function catches std::exception, lets its locals unwind,
// and only then throws the JS Error.

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "ingest.hpp"
#include "ml_bridge.hpp"

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

}  // namespace

EMSCRIPTEN_BINDINGS(sig_bridge) {
    emscripten::function("ingestFile", &ingest_file);
    emscripten::function("classifySpectrogram", &classify_spectrogram);
}
