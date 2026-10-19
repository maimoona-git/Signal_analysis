#include "wasm_worker_backend.hpp"

#include <emscripten.h>

#include <QString>

#include "error_text.hpp"
#include "pipeline_io.hpp"

namespace sigui {

// Copies the capture and options out of WASM memory (slice() makes buffers the page can transfer
// to the worker without another copy) and hands them to the page. Returns 1 if the worker took
// the job, 0 if it cannot run one.
EM_JS(int, sig_worker_run_js, (int id, const uint8_t* bytes, size_t length, const char* filename,
                               const uint8_t* options, size_t options_length), {
    if (!window.sigWorkerRun) return 0;
    const data = HEAPU8.slice(bytes, bytes + length);
    const opts = HEAPU8.slice(options, options + options_length);
    return window.sigWorkerRun(id, data, UTF8ToString(filename), opts) ? 1 : 0;
});

WorkerBackend* WorkerBackend::instance_ = nullptr;

WorkerBackend::WorkerBackend() { instance_ = this; }

WorkerBackend::~WorkerBackend() {
    if (instance_ == this) instance_ = nullptr;
}

void WorkerBackend::run(const AnalysisRequest& request, AnalysisCallback done) {
    const std::vector<uint8_t> options = sigapp::serialize_options(request.options);
    const int id = next_id_++;
    const std::string filename = request.filename.toStdString();
    pending_.emplace(id, Pending{request, std::move(done)});
    const int accepted = sig_worker_run_js(id, reinterpret_cast<const uint8_t*>(request.bytes.constData()),
                                           static_cast<size_t>(request.bytes.size()), filename.c_str(),
                                           options.data(), options.size());
    if (!accepted) complete(id, {}, "the analysis worker is not available", /*unavailable=*/true);
}

void WorkerBackend::finished(int id, std::vector<uint8_t> blob, const std::string& error, bool unavailable) {
    if (instance_) instance_->complete(id, std::move(blob), error, unavailable);
}

void WorkerBackend::complete(int id, std::vector<uint8_t> blob, const std::string& error, bool unavailable) {
    auto it = pending_.find(id);
    if (it == pending_.end()) return;  // already answered
    Pending pending = std::move(it->second);
    pending_.erase(it);

    if (unavailable) {
        try {
            fallback_.run(pending.request, std::move(pending.done));  // the same analysis, in this module
        } catch (const std::exception& e) {
            // the in-module path reports a bad file synchronously, like WasmBackend always has
            pending.done(false, nullptr, QString::fromStdString(sigbridge::describe_exception(e)));
        }
        return;
    }
    if (!error.empty()) {
        pending.done(false, nullptr, QString::fromStdString(error));
        return;
    }
    try {
        auto result = std::make_shared<const sigapp::PipelineResult>(sigapp::deserialize_result(blob.data(), blob.size()));
        pending.done(true, std::move(result), QString());
    } catch (const std::exception& e) {
        pending.done(false, nullptr, QString::fromStdString(sigbridge::describe_exception(e)));
    }
}

}  // namespace sigui
