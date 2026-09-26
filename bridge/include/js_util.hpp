#pragma once

// Small helpers for code that talks to JavaScript through Embind (WASM only),
// shared by the bridge and the Qt app.

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace sigbridge {

// Throws a real JS Error. Call it only after your C++ locals have been destroyed
// (a JS exception does not unwind C++ frames), e.g. from a catch block's tail.
[[noreturn]] inline void throw_js_error(const std::string& message) {
    emscripten::val::global("Error").new_(message).throw_();
}

// Copies an ArrayBuffer / typed array / DataView from JS into WASM memory.
// One bulk `TypedArray.set` (a memcpy), respecting byteOffset/byteLength so a
// subarray view of a larger buffer is read correctly.
inline std::vector<uint8_t> bytes_from_js(const emscripten::val& v) {
    const emscripten::val array_buffer = emscripten::val::global("ArrayBuffer");
    const emscripten::val uint8 = emscripten::val::global("Uint8Array");
    emscripten::val view = emscripten::val::undefined();
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
        emscripten::val(emscripten::typed_memory_view(n, out.data())).call<void>("set", view);
    }
    return out;
}

}  // namespace sigbridge
