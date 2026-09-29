#include "channel_finder.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "dsp.hpp"

namespace sigcore {

namespace {

// Median of a copy (the input PSD itself is left untouched) -- robust to a
// handful of strong occupied bins skewing a plain mean, same reasoning as
// estimate_symbol_rate's own noise-floor handling (symbol_rate.cpp).
float median_of(std::vector<float> v) {
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return (n % 2 == 1) ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

}  // namespace

ChannelFinderResult find_channels(const std::vector<std::complex<float>>& samples, double sample_rate,
                                   unsigned int nfft, float threshold_db, unsigned int merge_gap_bins,
                                   unsigned int min_bins, size_t max_channels) {
    if (samples.empty()) {
        throw std::runtime_error("find_channels: samples is empty");
    }
    if (nfft < 2) {
        throw std::runtime_error("find_channels: nfft must be >= 2");
    }
    if (sample_rate <= 0.0) {
        throw std::runtime_error("find_channels: sample_rate must be > 0");
    }

    const auto psd = compute_psd(samples, nfft);  // fft-shifted dB, bin nfft/2 == DC
    const float floor_db = median_of(psd);
    const double freq_res = sample_rate / static_cast<double>(nfft);

    std::vector<bool> occupied(nfft);
    for (unsigned int k = 0; k < nfft; ++k) occupied[k] = (psd[k] - floor_db) > threshold_db;

    struct Run {
        unsigned int start, len;
        float peak_db;
    };

    // Pass 1: raw contiguous occupied runs, off the untouched threshold mask.
    std::vector<Run> raw_runs;
    unsigned int run_start = 0;
    bool in_run = false;
    float run_peak = -1e30f;
    for (unsigned int k = 0; k <= nfft; ++k) {
        const bool on = (k < nfft) && occupied[k];
        if (on && !in_run) {
            in_run = true;
            run_start = k;
            run_peak = psd[k];
        } else if (on && in_run) {
            run_peak = std::max(run_peak, psd[k]);
        } else if (!on && in_run) {
            raw_runs.push_back({run_start, k - run_start, run_peak});
            in_run = false;
        }
    }

    // Pass 2: bridge runs separated by a small gap, so one channel with a
    // shallow mid-band dip is not reported as two separate ones. Working from
    // the fixed list of raw runs (not mutating the bin mask while scanning
    // it) keeps this order-independent.
    std::vector<Run> runs;
    for (const auto& r : raw_runs) {
        if (!runs.empty()) {
            const unsigned int gap = r.start - (runs.back().start + runs.back().len);
            if (gap <= merge_gap_bins) {
                runs.back().len = (r.start + r.len) - runs.back().start;
                runs.back().peak_db = std::max(runs.back().peak_db, r.peak_db);
                continue;
            }
        }
        runs.push_back(r);
    }
    runs.erase(std::remove_if(runs.begin(), runs.end(), [&](const Run& r) { return r.len < min_bins; }),
               runs.end());

    std::sort(runs.begin(), runs.end(), [](const Run& a, const Run& b) { return a.peak_db > b.peak_db; });
    if (runs.size() > max_channels) runs.resize(max_channels);

    ChannelFinderResult result;
    result.noise_floor_db = floor_db;
    if (runs.empty()) return result;

    const float strongest_peak = runs.front().peak_db;
    for (const auto& r : runs) {
        ChannelCandidate c;
        const double center_bin = static_cast<double>(r.start) + static_cast<double>(r.len) / 2.0;
        c.center_freq_hz = (center_bin - static_cast<double>(nfft) / 2.0) * freq_res;
        c.bandwidth_hz = static_cast<double>(r.len) * freq_res;
        // Relative to the strongest channel found, not an absolute floor --
        // see ChannelCandidate::confidence's own comment.
        const float span = std::max(strongest_peak - floor_db, 1e-3f);
        c.confidence = std::clamp((r.peak_db - floor_db) / span, 0.0f, 1.0f);
        result.channels.push_back(c);
    }
    return result;
}

ExtractedChannel extract_channel(const std::vector<std::complex<float>>& samples, double sample_rate,
                                  const ChannelCandidate& channel) {
    if (samples.empty()) {
        throw std::runtime_error("extract_channel: samples is empty");
    }
    if (sample_rate <= 0.0) {
        throw std::runtime_error("extract_channel: sample_rate must be > 0");
    }
    if (!(channel.bandwidth_hz > 0.0) || channel.bandwidth_hz >= sample_rate) {
        throw std::runtime_error("extract_channel: bandwidth_hz must be > 0 and < sample_rate");
    }

    // Mix the channel's centre down to DC.
    std::vector<std::complex<float>> mixed(samples.size());
    for (size_t i = 0; i < samples.size(); ++i) {
        const float phase =
            -2.0f * static_cast<float>(M_PI) * static_cast<float>(channel.center_freq_hz / sample_rate) *
            static_cast<float>(i);
        mixed[i] = samples[i] * std::complex<float>(std::cos(phase), std::sin(phase));
    }

    // Lowpass to a margin beyond the channel's own half-bandwidth, clamped short
    // of Nyquist so the Kaiser design always has a real transition band, then
    // decimate down to a rate a few times the channel's own bandwidth -- both
    // margins found by measuring (not assumed):
    //  - The filter's own cutoff needs headroom past the DETECTED bandwidth,
    //    not to sit exactly at it: a blindly-detected bandwidth (occupancy
    //    above a noise-floor threshold) is necessarily a bit narrower than a
    //    real pulse-shaped signal's true edge, and clipping that edge distorts
    //    the pulse shape before it ever reaches recover_linear's own RRC
    //    matched filter -- a mismatched-filter problem, the same lesson
    //    PulseShape::Rect's own comment describes for the wrong filter FAMILY,
    //    here from the wrong filter WIDTH instead. Confirmed directly: with no
    //    margin here, a real embedded message that decoded cleanly failed
    //    almost totally (measured ~43% bit errors, chance level) even at an
    //    exactly correct, integer samples_per_symbol.
    //  - The decimation ratio separately needs headroom because the caller
    //    only learns samples_per_symbol AFTER extraction, via
    //    estimate_symbol_rate on the result -- a mismatched bandwidth
    //    detection can leave that a non-integer ratio (e.g. 4.8, not 5), and
    //    even a few percent of that was enough to fully desync
    //    recover_linear's own fixed-rate symbol synchronizer over a real
    //    message's length, not just add noise.
    // These are deliberately generous margins, not a tight brick-wall fit.
    constexpr double kFilterMargin = 1.4;
    constexpr double kDecimationMargin = 3.0;
    const float fc =
        std::clamp(static_cast<float>((channel.bandwidth_hz * kFilterMargin / 2.0) / sample_rate), 1e-4f, 0.45f);
    // More decimation (a narrower channel within a wider capture) needs a
    // sharper transition to reject everything outside it -- scale taps with
    // the oversampling ratio, capped for performance the same way this
    // project caps other unbounded-with-input-size costs (e.g. symbol_rate.
    // cpp's kMaxFftSize).
    const unsigned int n_taps =
        std::clamp<unsigned int>(static_cast<unsigned int>(sample_rate / std::max(channel.bandwidth_hz, 1.0)) * 8,
                                  32, 2048);
    const auto filtered = lowpass_filter(mixed, n_taps, fc, 60.0f);

    const unsigned int decim =
        std::max<unsigned int>(1, static_cast<unsigned int>(sample_rate / (channel.bandwidth_hz * kDecimationMargin)));

    ExtractedChannel result;
    result.samples.reserve(filtered.size() / decim + 1);
    for (size_t i = 0; i < filtered.size(); i += decim) result.samples.push_back(filtered[i]);
    result.sample_rate = sample_rate / static_cast<double>(decim);
    return result;
}

}  // namespace sigcore
