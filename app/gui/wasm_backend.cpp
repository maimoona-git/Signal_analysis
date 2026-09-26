#include "wasm_backend.hpp"

#include "pipeline_bridge.hpp"

namespace sigui {

void WasmBackend::run(const AnalysisRequest& request, AnalysisCallback done) {
    sigbridge::run_pipeline_async(
        reinterpret_cast<const uint8_t*>(request.bytes.constData()), static_cast<size_t>(request.bytes.size()),
        request.filename.toStdString(), request.options,
        [done = std::move(done)](bool ok, const sigapp::PipelineResult& result, const std::string& error) {
            done(ok, ok ? std::make_shared<const sigapp::PipelineResult>(result) : nullptr,
                 QString::fromStdString(error));
        });
}

}  // namespace sigui
