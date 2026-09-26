#pragma once

// C++ -> JS ML call-out (WASM only).
//
// The pipeline is synchronous C++ but onnxruntime-web inference is async, and
// there is no way to block a WASM frame on a Promise without Asyncify. Linking
// Asyncify into the whole Qt app instruments every function (bigger, slower),
// so the bridge is continuation-passing instead: C++ starts the call, hands
// over a callback, and JS calls back into C++ when the Promise settles.
//
//   classify_async(spec, rows, cols, done)
//     C++  -> JS   Module.sigMlClassifier(Float32Array spec, rows, cols)
//     JS   -> C++  Module.sigMlComplete(id, ok, payload)  ->  done(...)
//
// `done` is always invoked later from a JS microtask, never re-entrantly from
// inside classify_async, including when there is no classifier registered.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ml_result.hpp"
#include "signal_data.hpp"

namespace sigbridge {

// ok == true: `result` is valid. ok == false: `error` says why (the classifier
// threw or rejected, returned something malformed, or none is registered).
using MlCallback = std::function<void(bool ok, const MlResult& result, const std::string& error)>;

// `spectrogram` is a row-major rows x cols float image; throws
// std::invalid_argument if its size is not rows * cols. The data is copied
// before returning, so the caller's vector may be destroyed immediately.
void classify_async(const std::vector<float>& spectrogram, unsigned int rows, unsigned int cols,
                    MlCallback done);

// The pipeline's ML step: builds the classifier's input image from a capture
// (sigcore::make_spectrogram_image, the C++ port of the training-time
// preprocessing) and hands it to classify_async. Throws std::runtime_error
// synchronously if the capture is too short to preprocess; every later failure
// arrives through `done`.
void classify_signal(const sigcore::SignalData& signal, MlCallback done);

// Number of classify_async calls whose callback has not fired yet (for leak
// checks: it must return to 0 once every Promise has settled).
std::size_t pending_count();

}  // namespace sigbridge
