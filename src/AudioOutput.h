#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Float audio output through miniaudio (WASAPI on Windows, ALSA/PulseAudio on Linux), stereo or 7.1 surround.
// miniaudio.h stays out of this header because it is several megabytes.
class AudioOutput {
public:
    // The channels the callback fills: stereo (front left, front right), or 7.1 surround in the order WASAPI and
    // miniaudio give it: front left, front right, centre, LFE, back (rear) left, back right, side left, side right.
    // miniaudio mixes them into the device's own channels where it has others (a stereo or 5.1 device).
    enum class Layout { Stereo, Surround71 };
    static constexpr uint32_t kSurroundChannels = 8;
    static uint32_t channelCount(Layout layout) { return layout == Layout::Surround71 ? kSurroundChannels : 2; }

    // Called on the audio thread; must fill frames * channelCount(layout) interleaved floats.
    using RenderCallback = void (*)(void* user, float* interleaved, uint32_t frames);

    // nullBackend: miniaudio's null device, which pulls samples in real time and plays nothing (for tests).
    explicit AudioOutput(bool nullBackend = false);
    ~AudioOutput();
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    std::vector<std::string> listDevices();

    // Opens a device without starting it, so the caller can size its renderer to sampleRate() first.
    // An empty name selects the system default device; sampleRate 0 keeps the device's native rate.
    bool open(const std::string& deviceName, uint32_t sampleRate, uint32_t periodFrames, Layout layout,
              RenderCallback callback, void* user, std::string& error);
    bool start(std::string& error);
    void close();

    bool isRunning() const;
    uint32_t sampleRate() const;
    uint32_t periodFrames() const;  // Actual period size chosen by the backend
    uint32_t channels() const;      // The callback's (0 when closed)
    // The device's own channels, and their speakers ("FL FR FC LFE BL BR SL SR"), which may differ from ours.
    uint32_t deviceChannels() const;
    std::string deviceSpeakers() const;
    std::string speakers() const;   // Ours, likewise
    std::string deviceName() const;
    std::string backendName() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
