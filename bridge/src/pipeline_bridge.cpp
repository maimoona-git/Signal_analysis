#include "pipeline_bridge.hpp"

#include <algorithm>
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
                // Gap-list Phase 2.2: hand over the runner-up labels (top 3 overall), not
                // just the top one, so pipeline_finish can retry them.
                sigapp::Classification classification{ml.label, ml.confidence, {}};
                std::vector<std::size_t> order(ml.probabilities.size());
                for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
                std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
                    return ml.probabilities[a] > ml.probabilities[b];
                });
                for (std::size_t k = 0; k < order.size() && classification.alternatives.size() < 2; ++k) {
                    if (order[k] == ml.top_index) continue;
                    classification.alternatives.push_back({ml.classes[order[k]], ml.probabilities[order[k]], {}});
                }
                result = std::make_shared<const sigapp::PipelineResult>(
                    sigapp::pipeline_finish(std::move(*prep), classification, options));
            } catch (const std::exception& e) {
                done(false, nullptr, describe_exception(e));
                return;
            }
            done(true, std::move(result), "");
        });
}

}  // namespace sigbridge
