#include "ml_bridge.hpp"

#include <emscripten.h>
#include <emscripten/console.h>
#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <stdexcept>
#include <unordered_map>

using emscripten::val;

namespace sigbridge {

namespace {

std::unordered_map<uint32_t, MlCallback>& pending() {
    static std::unordered_map<uint32_t, MlCallback> calls;
    return calls;
}
uint32_t g_next_id = 1;

// Runs the registered JS classifier and reports back via Module.sigMlComplete.
// Everything that can throw runs inside a Promise chain here, in JS: a JS
// exception cannot be caught by C++ across the WASM boundary, so it has to be
// turned into an (ok=false, message) completion on this side.
EM_JS(void, sig_ml_start, (uint32_t id, const float* spec, int rows, int cols), {
    const n = rows * cols;
    const start = spec >>> 2;
    // Copy out of the WASM heap: the heap can grow (detaching views) and the
    // C++ buffer is freed as soon as classify_async returns.
    const input = HEAPF32.slice(start, start + n);

    // Contract for the classifier's return value (checked here so C++ only
    // ever sees well-formed arrays): { classes: iterable, probabilities: iterable }.
    const normalize = (r) => {
        if (r === null || typeof r !== 'object') {
            throw new Error('classifier must return an object {classes, probabilities}');
        }
        for (const key of ['classes', 'probabilities']) {
            if (r[key] == null || typeof r[key][Symbol.iterator] !== 'function') {
                throw new Error('classifier result.' + key + ' must be an array');
            }
        }
        return { classes: Array.from(r.classes, String), probabilities: Array.from(r.probabilities, Number) };
    };

    Promise.resolve()
        .then(() => {
            const fn = Module['sigMlClassifier'];
            if (typeof fn !== 'function') {
                throw new Error('no ML classifier registered (set Module.sigMlClassifier)');
            }
            return fn(input, rows, cols);
        })
        .then(normalize)
        .then(
            (payload) => Module['sigMlComplete'](id, true, payload),
            (e) => Module['sigMlComplete'](id, false, (e && e.message) ? String(e.message) : String(e)));
});

// Called from JS when a classifier Promise settles. Never lets an exception
// escape into the JS microtask queue.
void ml_complete(uint32_t id, bool ok, val payload) {
    auto it = pending().find(id);
    if (it == pending().end()) {
        return;  // already completed / unknown id
    }
    MlCallback done = std::move(it->second);
    pending().erase(it);

    bool success = false;
    MlResult result;
    std::string error;
    try {
        if (ok) {
            result = make_ml_result(emscripten::vecFromJSArray<std::string>(payload["classes"]),
                                    emscripten::convertJSArrayToNumberVector<float>(payload["probabilities"]));
            success = true;
        } else {
            error = payload.as<std::string>();
        }
    } catch (const std::exception& e) {
        error = e.what();
    }

    try {
        done(success, result, error);
    } catch (const std::exception& e) {
        emscripten_console_error(e.what());
    }
}

}  // namespace

void classify_async(const std::vector<float>& spectrogram, unsigned int rows, unsigned int cols,
                    MlCallback done) {
    if (static_cast<size_t>(rows) * cols != spectrogram.size()) {
        throw std::invalid_argument("classify_async: spectrogram size != rows * cols");
    }
    const uint32_t id = g_next_id++;
    pending()[id] = std::move(done);
    sig_ml_start(id, spectrogram.data(), static_cast<int>(rows), static_cast<int>(cols));
}

std::size_t pending_count() { return pending().size(); }

EMSCRIPTEN_BINDINGS(sig_ml_bridge) {
    emscripten::function("sigMlComplete", &ml_complete);
    emscripten::function("mlPendingCount", &pending_count);
}

}  // namespace sigbridge
