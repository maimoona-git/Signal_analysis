#pragma once

// The browser's default AnalysisBackend (WASM only): the pipeline -- ingest, ML classification,
// every search -- runs in the page's analysis Web Worker (web/js/analysis-worker.js, a second
// WASM instance), so the Qt UI stays responsive however long an analysis takes. Options go over
// and the result comes back in the binary format of app/pipeline/pipeline_io.hpp.
//
// If the page cannot run a worker (the browser lacks it, it failed to start), a run falls back
// to WasmBackend: the same analysis inside this module, blocking the UI while it runs, exactly
// as before the worker existed.

#include <functional>
#include <map>
#include <memory>

#include "analysis_backend.hpp"
#include "wasm_backend.hpp"

namespace sigui {

class WorkerBackend : public AnalysisBackend {
public:
    WorkerBackend();
    ~WorkerBackend() override;
    void run(const AnalysisRequest& request, AnalysisCallback done) override;

    // Called (through Embind, see wasm_bindings.cpp) when the page has an answer for run `id`:
    // `blob` is the serialized result, or empty with `error` set. `unavailable` means the worker
    // itself is gone and the run should be repeated in-module.
    static void finished(int id, std::vector<uint8_t> blob, const std::string& error, bool unavailable);

private:
    struct Pending {
        AnalysisRequest request;  // kept so an unavailable worker can fall back
        AnalysisCallback done;
    };
    void complete(int id, std::vector<uint8_t> blob, const std::string& error, bool unavailable);

    WasmBackend fallback_;
    std::map<int, Pending> pending_;
    int next_id_ = 1;
    static WorkerBackend* instance_;
};

}  // namespace sigui
