#include "pipeline_bridge.hpp"

#include <memory>
#include <stdexcept>

#include "error_text.hpp"
#include "ml_bridge.hpp"

namespace sigbridge {

void run_pipeline_async(const uint8_t* data, size_t size, const std::string& filename,
                        const sigapp::PipelineOptions& options, PipelineCallback done) {
    // Ingest + spectrogram now (errors surface synchronously), the rest after the ML call.
    auto prep = std::make_shared<sigapp::PipelinePrep>(sigapp::pipeline_prepare(data, size, filename, options));

    classify_signal(
        // sigcore::SignalData is what the ML preprocessing takes
        sigcore::SignalData{prep->signal.samples, prep->signal.sample_rate, prep->signal.format},
        [prep, options, done = std::move(done)](bool ok, const MlResult& ml, const std::string& error) {
            if (!ok) {
                done(false, nullptr, error);
                return;
            }
            std::shared_ptr<const sigapp::PipelineResult> result;
            try {
                result = std::make_shared<const sigapp::PipelineResult>(
                    sigapp::pipeline_finish(std::move(*prep), sigapp::Classification{ml.label, ml.confidence}, options));
            } catch (const std::exception& e) {
                done(false, nullptr, describe_exception(e));
                return;
            }
            done(true, std::move(result), "");
        });
}

}  // namespace sigbridge
