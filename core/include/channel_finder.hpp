#pragma once

#include <complex>
#include <vector>

namespace sigcore {

// One candidate occupied sub-band ("channel") found in a wideband capture.
struct ChannelCandidate {
    double center_freq_hz = 0.0;  // offset from DC (baseband centre); can be negative
    double bandwidth_hz = 0.0;
    // 0..1: how far the channel's peak clears the noise floor, relative to
    // the strongest channel found -- see find_channels's own comment. Not a
    // calibrated probability, the same convention as SymbolRateEstimate's own
    // confidence (see symbol_rate.hpp).
    double confidence = 0.0;
};

struct ChannelFinderResult {
    // Sorted strongest first (by peak height above the noise floor, not by
    // frequency). Empty, not an error, when nothing clears the threshold.
    std::vector<ChannelCandidate> channels;
    double noise_floor_db = 0.0;  // for diagnostics/display: the median PSD bin, in dB
};

// Blind detection of occupied sub-bands in a wideband capture, via PSD peak/
// plateau detection -- no signal-type assumptions, the same "blind" spirit as
// estimate_symbol_rate (symbol_rate.hpp). Real captures spanning more
// bandwidth than one signal occupies are exactly the PS's own stated case
// ("data recorded from different sensors and different locations"): this is
// the coarse first step of isolating which part of the spectrum to actually
// demodulate, via extract_channel below.
//
// Technique: compute_psd's own dB spectrum, in `nfft` bins; a bin's own
// median across the whole spectrum is the noise-floor estimate (robust to a
// few strong signals skewing a mean); a bin is "occupied" when it clears the
// floor by `threshold_db`. Contiguous occupied bins (small gaps up to
// `merge_gap_bins` are bridged, so one channel with a shallow mid-band dip is
// not reported as two) form one candidate; candidates narrower than
// `min_bins` are dropped as noise spikes, not real channels.
//
// Throws std::runtime_error for empty input, nfft < 2, or sample_rate <= 0.
ChannelFinderResult find_channels(const std::vector<std::complex<float>>& samples, double sample_rate,
                                   unsigned int nfft = 1024, float threshold_db = 10.0f,
                                   unsigned int merge_gap_bins = 2, unsigned int min_bins = 2,
                                   size_t max_channels = 8);

// One channel, isolated to baseband and decimated to a sample rate close to
// (but comfortably above, for a clean margin against the channel's own edge
// roll-off) its bandwidth: mix down by -center_freq_hz, lowpass filter to
// bandwidth_hz/2, then decimate. The output can be fed into the rest of this
// project's pipeline exactly like a capture that was narrowband to begin
// with (its own sample_rate, in ExtractedChannel::sample_rate, is what a
// caller should then pass as PipelineOptions::sample_rate).
struct ExtractedChannel {
    std::vector<std::complex<float>> samples;
    double sample_rate = 0.0;
};

// Throws std::runtime_error if `channel.bandwidth_hz` is not a sensible
// positive fraction of `sample_rate` (i.e. would not leave room for any
// lowpass filter transition band at all).
ExtractedChannel extract_channel(const std::vector<std::complex<float>>& samples, double sample_rate,
                                  const ChannelCandidate& channel);

}  // namespace sigcore
