#include "AudioOutput.h"

#include "miniaudio.h"

namespace {

struct CallbackTarget {
    AudioOutput::RenderCallback callback = nullptr;
    void* user = nullptr;
};

void dataCallback(ma_device* device, void* output, const void* /*input*/, ma_uint32 frames) {
    const CallbackTarget* target = static_cast<const CallbackTarget*>(device->pUserData);
    target->callback(target->user, static_cast<float*>(output), frames);
}

// 7.1 surround as WASAPI's KSAUDIO_SPEAKER_7POINT1_SURROUND orders it (and miniaudio's standard map for 8 channels).
const ma_channel kSurroundMap[AudioOutput::kSurroundChannels] = {
    MA_CHANNEL_FRONT_LEFT, MA_CHANNEL_FRONT_RIGHT, MA_CHANNEL_FRONT_CENTER, MA_CHANNEL_LFE,
    MA_CHANNEL_BACK_LEFT,  MA_CHANNEL_BACK_RIGHT,  MA_CHANNEL_SIDE_LEFT,    MA_CHANNEL_SIDE_RIGHT,
};
const ma_channel kStereoMap[2] = {MA_CHANNEL_FRONT_LEFT, MA_CHANNEL_FRONT_RIGHT};

std::string speakerNames(const ma_channel* map, ma_uint32 channels) {
    std::string names;
    for (ma_uint32 i = 0; i < channels; i++) {
        const char* name = "?";
        switch (map[i]) {
        case MA_CHANNEL_MONO: name = "M"; break;
        case MA_CHANNEL_FRONT_LEFT: name = "FL"; break;
        case MA_CHANNEL_FRONT_RIGHT: name = "FR"; break;
        case MA_CHANNEL_FRONT_CENTER: name = "FC"; break;
        case MA_CHANNEL_LFE: name = "LFE"; break;
        case MA_CHANNEL_BACK_LEFT: name = "BL"; break;
        case MA_CHANNEL_BACK_RIGHT: name = "BR"; break;
        case MA_CHANNEL_FRONT_LEFT_CENTER: name = "FLC"; break;
        case MA_CHANNEL_FRONT_RIGHT_CENTER: name = "FRC"; break;
        case MA_CHANNEL_BACK_CENTER: name = "BC"; break;
        case MA_CHANNEL_SIDE_LEFT: name = "SL"; break;
        case MA_CHANNEL_SIDE_RIGHT: name = "SR"; break;
        default: break;
        }
        if (!names.empty()) names += ' ';
        names += name;
    }
    return names;
}

}  // namespace

struct AudioOutput::Impl {
    ma_context context;
    bool contextReady = false;
    ma_device device;
    bool deviceReady = false;
    CallbackTarget target;
};

AudioOutput::AudioOutput(bool nullBackend) : impl_(new Impl) {
    const ma_context_config config = ma_context_config_init();
    const ma_backend nullOnly[] = {ma_backend_null};
    impl_->contextReady = ma_context_init(nullBackend ? nullOnly : nullptr, nullBackend ? 1 : 0, &config, &impl_->context) == MA_SUCCESS;
}

AudioOutput::~AudioOutput() {
    close();
    if (impl_->contextReady) ma_context_uninit(&impl_->context);
}

std::vector<std::string> AudioOutput::listDevices() {
    std::vector<std::string> names;
    if (!impl_->contextReady) return names;
    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(&impl_->context, &infos, &count, nullptr, nullptr) != MA_SUCCESS) return names;
    for (ma_uint32 i = 0; i < count; i++) names.push_back(infos[i].name);
    return names;
}

bool AudioOutput::open(const std::string& deviceName, uint32_t sampleRate, uint32_t periodFrames, Layout layout,
                       RenderCallback callback, void* user, std::string& error) {
    close();
    if (!impl_->contextReady) {
        error = "No audio backend is available";
        return false;
    }

    // A device that has disappeared (e.g. unplugged USB audio) falls back to the system default.
    ma_device_id deviceId;
    bool haveDeviceId = false;
    if (!deviceName.empty()) {
        ma_device_info* infos = nullptr;
        ma_uint32 count = 0;
        if (ma_context_get_devices(&impl_->context, &infos, &count, nullptr, nullptr) == MA_SUCCESS) {
            for (ma_uint32 i = 0; i < count && !haveDeviceId; i++) {
                if (deviceName == infos[i].name) {
                    deviceId = infos[i].id;
                    haveDeviceId = true;
                }
            }
        }
    }

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.pDeviceID = haveDeviceId ? &deviceId : nullptr;
    config.playback.format = ma_format_f32;
    // Our channels by speaker: miniaudio maps them onto the device's by position, and mixes those it lacks into the
    // nearest it has.
    config.playback.channels = channelCount(layout);
    config.playback.pChannelMap = const_cast<ma_channel*>(layout == Layout::Surround71 ? kSurroundMap : kStereoMap);
    config.sampleRate = sampleRate;
    config.periodSizeInFrames = periodFrames;
    config.performanceProfile = ma_performance_profile_low_latency;
    config.noPreSilencedOutputBuffer = MA_TRUE;  // The render callback always writes every frame
    config.dataCallback = dataCallback;
    impl_->target.callback = callback;
    impl_->target.user = user;
    config.pUserData = &impl_->target;

    const ma_result result = ma_device_init(&impl_->context, &config, &impl_->device);
    if (result != MA_SUCCESS) {
        error = std::string("Cannot open the audio device: ") + ma_result_description(result);
        return false;
    }
    impl_->deviceReady = true;
    return true;
}

bool AudioOutput::start(std::string& error) {
    if (!impl_->deviceReady) {
        error = "No audio device is open";
        return false;
    }
    const ma_result result = ma_device_start(&impl_->device);
    if (result != MA_SUCCESS) {
        error = std::string("Cannot start the audio device: ") + ma_result_description(result);
        return false;
    }
    return true;
}

void AudioOutput::close() {
    // Blocks until the audio thread has left the callback.
    if (impl_->deviceReady) {
        ma_device_uninit(&impl_->device);
        impl_->deviceReady = false;
    }
}

bool AudioOutput::isRunning() const {
    return impl_->deviceReady && ma_device_is_started(&impl_->device);
}

uint32_t AudioOutput::sampleRate() const {
    return impl_->deviceReady ? impl_->device.sampleRate : 0;
}

uint32_t AudioOutput::periodFrames() const {
    return impl_->deviceReady ? impl_->device.playback.internalPeriodSizeInFrames : 0;
}

uint32_t AudioOutput::channels() const {
    return impl_->deviceReady ? impl_->device.playback.channels : 0;
}

uint32_t AudioOutput::deviceChannels() const {
    return impl_->deviceReady ? impl_->device.playback.internalChannels : 0;
}

std::string AudioOutput::deviceSpeakers() const {
    if (!impl_->deviceReady) return "";
    return speakerNames(impl_->device.playback.internalChannelMap, impl_->device.playback.internalChannels);
}

std::string AudioOutput::speakers() const {
    if (!impl_->deviceReady) return "";
    return speakerNames(impl_->device.playback.channelMap, impl_->device.playback.channels);
}

std::string AudioOutput::deviceName() const {
    return impl_->deviceReady ? impl_->device.playback.name : "";
}

std::string AudioOutput::backendName() const {
    return impl_->contextReady ? ma_get_backend_name(impl_->context.backend) : "none";
}
