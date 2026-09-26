#pragma once

// The whole analysis pipeline as one asynchronous call (WASM only): ingest ->
// spectrogram -> ML classification (C++ -> JS -> C++, see ml_bridge.hpp) ->
// demod -> de-interleave -> FEC decode -> correlate. The synchronous stages are
// app/pipeline; this only stitches the asynchronous ML step between the two halves.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "pipeline.hpp"

namespace sigbridge {

// The result is shared, not copied: it holds the capture's samples and its spectrogram,
// several times the size of the file, so handing it around by value is not an option.
using PipelineCallback = std::function<void(bool ok, std::shared_ptr<const sigapp::PipelineResult> result,
                                            const std::string& error)>;

// Throws synchronously (std::invalid_argument / std::runtime_error) if the file
// cannot be ingested or is too short to analyse; everything after the ML call
// goes through `done`, which is always invoked later from a JS microtask. The
// bytes are copied before returning.
void run_pipeline_async(const uint8_t* data, size_t size, const std::string& filename,
                        const sigapp::PipelineOptions& options, PipelineCallback done);

}  // namespace sigbridge
