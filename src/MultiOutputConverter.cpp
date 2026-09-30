#include "MultiOutputConverter.h"

#include <algorithm>

#include "srchelper/srctools/include/FloatSampleProvider.h"
#include "srchelper/srctools/include/ResamplerModel.h"
#include "srchelper/srctools/include/ResamplerStage.h"
#include "srchelper/srctools/include/SincResampler.h"

namespace {

using SRCTools::FloatSample;
using SRCTools::FloatSampleProvider;

// A model from `source` to the output rate, as MT32Emu::SampleRateConverter builds its own (srchelper/InternalResampler).
FloatSampleProvider& createModel(MT32Emu::Synth& synth, FloatSampleProvider& source, double targetSampleRate,
                                 MT32Emu::SamplerateConversionQuality quality, std::vector<SRCTools::ResamplerStage*>& stages) {
    constexpr double kMaxAudibleFrequency = 20000.0;
    const double sourceSampleRate = synth.getStereoOutputSampleRate();
    if (sourceSampleRate == targetSampleRate) return source;  // The SampleRateConverter renders straight into the output then
    if (quality != MT32Emu::SamplerateConversionQuality_FASTEST) {
        const bool oversampledMode = sourceSampleRate == MT32Emu::Synth::getStereoOutputSampleRate(MT32Emu::AnalogOutputMode_OVERSAMPLED);
        if (oversampledMode && 0.5 * sourceSampleRate <= targetSampleRate) {
            const double passband = kMaxAudibleFrequency;
            const double stopband = 0.5 * sourceSampleRate + kMaxAudibleFrequency;
            SRCTools::ResamplerStage* stage = SRCTools::SincResampler::createSincResampler(
                sourceSampleRate, targetSampleRate, passband, stopband, SRCTools::ResamplerModel::DEFAULT_DB_SNR,
                SRCTools::ResamplerModel::DEFAULT_WINDOWED_SINC_MAX_UPSAMPLE_FACTOR);
            stages.push_back(stage);
            return SRCTools::ResamplerModel::createResamplerModel(source, *stage);
        }
    }
    return SRCTools::ResamplerModel::createResamplerModel(source, sourceSampleRate, targetSampleRate,
                                                          static_cast<SRCTools::ResamplerModel::Quality>(quality));
}

}  // namespace

// A part's stream, frames the mix's source rendered and its model has not taken yet.
class MultiOutputConverter::PartSource : public FloatSampleProvider {
public:
    PartSource() { queue.reserve(2 * 8192); }

    void getOutputSamples(FloatSample* outBuffer, unsigned int size) override {
        const size_t wanted = 2 * size_t(size);
        const size_t available = std::min(wanted, queue.size() - read);
        std::copy(queue.begin() + long(read), queue.begin() + long(read + available), outBuffer);
        std::fill(outBuffer + available, outBuffer + wanted, 0.0f);  // Never: the models pull alike
        read += available;
        if (read == queue.size()) {
            queue.clear();
            read = 0;
        }
    }

    std::vector<float> queue;  // Interleaved stereo
    size_t read = 0;
};

// Renders the synth as the SampleRateConverter's source does, one render() per pull, and queues the frames of each
// output of its own (the parts', then the MULTI pairs').
class MultiOutputConverter::MixSource : public FloatSampleProvider {
public:
    MixSource(MT32Emu::Synth& synth, std::vector<std::unique_ptr<PartSource>>& streams, int firstStream)
        : synth_(synth), streams_(streams), firstStream_(firstStream) {
        for (int stream = firstStream_; stream < kStreams; stream++) buffers_[stream].resize(2 * 4096);
    }

    void getOutputSamples(FloatSample* outBuffer, unsigned int size) override {
        float* streams[kStreams] = {};
        for (int stream = firstStream_; stream < kStreams; stream++) {
            std::vector<float>& buffer = buffers_[size_t(stream)];
            if (buffer.size() < 2 * size_t(size)) buffer.resize(2 * size_t(size));
            streams[stream] = buffer.data();
        }
        synth_.render(outBuffer, size, streams, streams + kParts);
        for (int stream = firstStream_; stream < kStreams; stream++) {
            std::vector<float>& queue = streams_[size_t(stream)]->queue;
            queue.insert(queue.end(), streams[stream], streams[stream] + 2 * size_t(size));
        }
    }

private:
    MT32Emu::Synth& synth_;
    std::vector<std::unique_ptr<PartSource>>& streams_;
    const int firstStream_;
    std::vector<float> buffers_[kStreams];
};

MultiOutputConverter::MultiOutputConverter(MT32Emu::Synth& synth, double outputSampleRate, MT32Emu::SamplerateConversionQuality quality,
                                           bool partStreams)
    : synthToOutputRatio_(double(MT32Emu::SAMPLE_RATE) / outputSampleRate), firstStream_(partStreams ? 0 : kParts) {
    for (int stream = 0; stream < kStreams; stream++) partSources_.emplace_back(new PartSource);
    mixSource_.reset(new MixSource(synth, partSources_, firstStream_));
    mixModel_ = &createModel(synth, *mixSource_, outputSampleRate, quality, stages_);
    partModels_.assign(size_t(kStreams), nullptr);
    for (int stream = firstStream_; stream < kStreams; stream++) {
        partModels_[size_t(stream)] = &createModel(synth, *partSources_[size_t(stream)], outputSampleRate, quality, stages_);
    }
    dropped_.resize(2 * 4096);
}

MultiOutputConverter::~MultiOutputConverter() {
    SRCTools::ResamplerModel::freeResamplerModel(*mixModel_, *mixSource_);
    for (int stream = firstStream_; stream < kStreams; stream++) {
        SRCTools::ResamplerModel::freeResamplerModel(*partModels_[size_t(stream)], *partSources_[size_t(stream)]);
    }
    for (SRCTools::ResamplerStage* stage : stages_) delete stage;
}

void MultiOutputConverter::getOutputSamples(float* mix, float* const* parts, float* const* multi, uint32_t frames) {
    mixModel_->getOutputSamples(mix, frames);  // Renders the synth; the streams' queues fill with the same frames
    if (dropped_.size() < 2 * size_t(frames)) dropped_.resize(2 * size_t(frames));
    for (int stream = 0; stream < kStreams; stream++) {
        float* wanted = stream < kParts ? (parts != nullptr ? parts[stream] : nullptr) : (multi != nullptr ? multi[stream - kParts] : nullptr);
        if (partModels_[size_t(stream)] == nullptr) {
            if (wanted != nullptr) std::fill(wanted, wanted + 2 * size_t(frames), 0.0f);  // Not carried: silent
            continue;
        }
        partModels_[size_t(stream)]->getOutputSamples(wanted != nullptr ? wanted : dropped_.data(), frames);
    }
}
