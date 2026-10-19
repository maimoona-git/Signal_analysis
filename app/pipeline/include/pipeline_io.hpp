#pragma once

// Gap-list Phase 4 (Web Worker): a binary wire format for PipelineOptions, Classification and
// PipelineResult, so the analysis can run in a second WASM instance inside a Web Worker and
// its result can come back into the Qt app as one transferable buffer (see
// web/js/analysis-worker.mjs). Portable C++, so the round trip is unit-tested natively.
//
// Little-endian, no versioning (both ends are always built from the same source). Reading is
// fully bounds-checked: truncated or malformed bytes throw std::runtime_error, never read
// out of range. Every field of the three structs is carried EXCEPT PipelineOptions::deadline,
// which is an in-process clock reading that means nothing in another instance (the receiver
// starts its own time budget from time_budget_sec).
//
// Adding a field to any of these structs means adding it to pipeline_io.cpp too;
// tests/core/test_pipeline_io.cpp fills every field with a distinct value, so it fails if one
// is left out.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "pipeline.hpp"

namespace sigapp {

std::vector<uint8_t> serialize_options(const PipelineOptions& options);
PipelineOptions deserialize_options(const uint8_t* data, size_t size);

std::vector<uint8_t> serialize_classification(const Classification& classification);
Classification deserialize_classification(const uint8_t* data, size_t size);

std::vector<uint8_t> serialize_result(const PipelineResult& result);
PipelineResult deserialize_result(const uint8_t* data, size_t size);

}  // namespace sigapp
