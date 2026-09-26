#pragma once

// How the main window gets an analysis done. The GUI does not know whether the
// pipeline runs synchronously in-process (tests) or asynchronously with the ML
// classification going out to onnxruntime-web in the browser (the WASM app).

#include <QByteArray>
#include <QString>

#include <functional>
#include <memory>

#include "pipeline.hpp"

namespace sigui {

struct AnalysisRequest {
    QByteArray bytes;                 // the capture as the browser handed it over
    QString filename;                 // decides .wav vs .iq, like the native app's path did
    sigapp::PipelineOptions options;
};

// ok == true: `result` is set. ok == false: `error` says why.
using AnalysisCallback =
    std::function<void(bool ok, std::shared_ptr<const sigapp::PipelineResult> result, const QString& error)>;

class AnalysisBackend {
public:
    virtual ~AnalysisBackend() = default;

    // Starts an analysis; `done` is called exactly once, possibly before run()
    // returns or much later. May throw std::exception for an immediate failure
    // (an unsupported file, say); the window reports that like any other error.
    virtual void run(const AnalysisRequest& request, AnalysisCallback done) = 0;
};

}  // namespace sigui
