#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "mt32emu.h"

namespace SRCTools {
class FloatSampleProvider;
class ResamplerStage;
}

// Resamples the synth's mix and its outputs of their own (MT32Emu::Synth::setPartOutputsAvailable: each part's, and the
// MULTI pairs') to the output rate together. Each stereo stream has a resampler model of its own, all built as
// MT32Emu::SampleRateConverter builds its one. A model's pulls from its source depend only on the output lengths asked
// of it, never on the samples, so each stream's model takes exactly the frames the mix's took, in the same pieces: the
// mix's source renders the synth (as the SampleRateConverter does, so the mix comes out the same) and keeps the other
// streams of those frames for their sources. Every model runs every time, heard or not, to stay in step.
class MultiOutputConverter {
public:
    static constexpr int kParts = 16;      // MT32Emu's MAX_PART_COUNT: parts 1-8 (0-7), rhythm (8), parts 9-15 (9-15)
    static constexpr int kMultiPairs = 3;  // The MULTI outputs 1-6 as stereo pairs (1+2, 3+4, 5+6): MULTI_OUTPUT_PAIRS
    static constexpr int kStreams = kParts + kMultiPairs;

    // Without `partStreams`, only the mix and the MULTI pairs (the parts' outputs come out silent).
    MultiOutputConverter(MT32Emu::Synth& synth, double outputSampleRate, MT32Emu::SamplerateConversionQuality quality, bool partStreams);
    ~MultiOutputConverter();
    MultiOutputConverter(const MultiOutputConverter&) = delete;
    MultiOutputConverter& operator=(const MultiOutputConverter&) = delete;

    // `frames` frames of the mix, of each part into parts[part number] (kParts entries) and of the MULTI pairs into
    // multi (kMultiPairs entries), interleaved stereo; null entries, or null arrays, are rendered and dropped.
    void getOutputSamples(float* mix, float* const* parts, float* const* multi, uint32_t frames);
    double convertOutputToSynthTimestamp(double outputTimestamp) const { return outputTimestamp * synthToOutputRatio_; }

private:
    class MixSource;
    class PartSource;

    const double synthToOutputRatio_;
    const int firstStream_;  // 0 with the part streams, else kParts: the first stream with a source and a model
    std::unique_ptr<MixSource> mixSource_;
    std::vector<std::unique_ptr<PartSource>> partSources_;  // For every stream; unused ones before firstStream_
    SRCTools::FloatSampleProvider* mixModel_ = nullptr;
    std::vector<SRCTools::FloatSampleProvider*> partModels_;
    std::vector<SRCTools::ResamplerStage*> stages_;  // Stages the models do not own
    std::vector<float> dropped_;                       // Where unwanted parts go
};
