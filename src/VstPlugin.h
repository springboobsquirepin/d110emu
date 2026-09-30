#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "PluginCore.h"
#include "Vst2.h"

// D110Emu as a VST 2.4 instrument: PluginCore (the App with the host's MIDI in, its audio out and the state in its
// project) behind the VST2 interface. Outputs: the mix (1-2), then a stereo pair per part (3-34: parts 1-8, rhythm,
// parts 9-15), which the App's Output menus put parts on, then MULTI 1-6 (35-40, mono), where parts and rhythm keys
// assigned to them play. The host's audio thread calls processReplacing and effProcessEvents; its UI thread everything
// else.
class VstPlugin {
public:
    explicit VstPlugin(vst2::HostCallback host);
    ~VstPlugin();
    VstPlugin(const VstPlugin&) = delete;
    VstPlugin& operator=(const VstPlugin&) = delete;

    vst2::AEffect* effect() { return &effect_; }

    // The project state's format (PluginCore's), for tests.
    static std::vector<uint8_t> encodeState(const std::string& settings, const std::vector<uint8_t>& memory) {
        return PluginCore::encodeState(settings, memory);
    }
    static bool decodeState(const uint8_t* data, size_t size, std::string& settings, std::vector<uint8_t>& memory) {
        return PluginCore::decodeState(data, size, settings, memory);
    }

private:
    static intptr_t VST2_CALL dispatcherProc(vst2::AEffect* effect, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt);
    static void VST2_CALL processReplacingProc(vst2::AEffect* effect, float** inputs, float** outputs, int32_t frames);
    static void VST2_CALL processProc(vst2::AEffect* effect, float** inputs, float** outputs, int32_t frames);
    static void VST2_CALL setParameterProc(vst2::AEffect* effect, int32_t index, float value);
    static float VST2_CALL getParameterProc(vst2::AEffect* effect, int32_t index);

    intptr_t dispatch(int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt);
    void queueEvents(const vst2::VstEvents& events);
    void process(float** outputs, int32_t frames, bool accumulate);

    vst2::AEffect effect_;
    vst2::HostCallback host_;
    PluginCore core_;
    std::unique_ptr<PluginEditor> editor_;
    vst2::ERect editorRect_{};
    std::vector<uint8_t> chunk_;  // The last effGetChunk's data, which the host reads until the next call
};
