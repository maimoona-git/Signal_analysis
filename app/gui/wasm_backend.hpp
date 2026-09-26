#pragma once

// The browser's AnalysisBackend (WASM only): runs the pipeline in this module and
// calls out to the page's JavaScript (onnxruntime-web) for the ML classification,
// through the bridge (sigbridge::run_pipeline_async).

#include "analysis_backend.hpp"

namespace sigui {

class WasmBackend : public AnalysisBackend {
public:
    void run(const AnalysisRequest& request, AnalysisCallback done) override;
};

}  // namespace sigui
