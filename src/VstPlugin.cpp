// D110Emu as a VST 2.4 instrument (see VstPlugin.h): the host's side of the plugin, without the window.

#include "VstPlugin.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

namespace {

constexpr int32_t kUniqueId = int32_t((uint32_t('D') << 24) | (uint32_t('1') << 16) | (uint32_t('1') << 8) | uint32_t('m'));
constexpr int32_t kVendorVersion = 1000;  // 1.0.0.0
const char* const kEffectName = "D110Emu";
const char* const kVendor = "D110Emu";
const char* const kProduct = "D110Emu - Roland D-110 emulator";
const char* const kProgramName = "D-110";
constexpr int32_t kPartPins = 2 + 2 * SynthEngine::kPartOutputs;          // The mix, then a pair per part
constexpr int32_t kOutputs = kPartPins + PluginCore::kMultiOutputs;         // Then MULTI 1-6, mono

// The name of a part's output, as the VST3 plugin's (parts 1-8, rhythm, parts 9-15 are part numbers 0-15).
std::string partOutputName(int part) {
    if (part == 8) return "Rhythm";
    return "Part " + std::to_string(part < 8 ? part + 1 : part);
}

void copyString(void* destination, const char* text, size_t size) {
    char* out = static_cast<char*>(destination);
    std::strncpy(out, text, size - 1);
    out[size - 1] = '\0';
}

intptr_t canDo(const char* what) {
    if (what == nullptr) return 0;
    const std::string feature(what);
    if (feature == "receiveVstEvents" || feature == "receiveVstMidiEvent" || feature == "receiveVstSysexEvent") return 1;
    if (feature == "sendVstEvents" || feature == "sendVstMidiEvent" || feature == "offline" || feature == "bypass" ||
        feature == "midiProgramNames" || feature == "plugAsChannelInsert" || feature == "plugAsSend" || feature == "mixDryWet") {
        return -1;
    }
    return 0;
}

}  // namespace

VstPlugin::VstPlugin(vst2::HostCallback host) : host_(host), core_(PluginFormat::Vst2) {
    std::memset(&effect_, 0, sizeof(effect_));  // Also the padding, which some hosts read as part of `flags`
    effect_.magic = vst2::kEffectMagic;
    effect_.dispatcher = &dispatcherProc;
    effect_.process = &processProc;
    effect_.setParameter = &setParameterProc;
    effect_.getParameter = &getParameterProc;
    effect_.numPrograms = 1;
    effect_.numParams = 0;
    effect_.numInputs = 0;
    effect_.numOutputs = kOutputs;
    effect_.flags = vst2::effFlagsCanReplacing | vst2::effFlagsProgramChunks | vst2::effFlagsIsSynth;
    effect_.ioRatio = 1.0f;
    effect_.object = this;
    effect_.uniqueID = kUniqueId;
    effect_.version = kVendorVersion;
    effect_.processReplacing = &processReplacingProc;
    editor_ = createPluginEditor(core_);
    if (editor_) {
        effect_.flags |= vst2::effFlagsHasEditor;
        editor_->setHostResize([this](int width, int height) {
            return host_ != nullptr && host_(&effect_, vst2::audioMasterSizeWindow, width, height, nullptr, 0.0f) != 0;
        });
        core_.setEditor(editor_.get());
    }
}

VstPlugin::~VstPlugin() {
    core_.setEditor(nullptr);
    editor_.reset();  // Closes the window; the core then closes the App
}

// ---------------------------------------------------------------------------------------------
// The host's calls

intptr_t VST2_CALL VstPlugin::dispatcherProc(vst2::AEffect* effect, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt) {
    VstPlugin* plugin = effect != nullptr ? static_cast<VstPlugin*>(effect->object) : nullptr;
    if (plugin == nullptr) return 0;
    if (opcode == vst2::effClose) {
        delete plugin;
        return 0;
    }
    return plugin->dispatch(opcode, index, value, ptr, opt);
}

void VST2_CALL VstPlugin::processReplacingProc(vst2::AEffect* effect, float** /*inputs*/, float** outputs, int32_t frames) {
    static_cast<VstPlugin*>(effect->object)->process(outputs, frames, false);
}

void VST2_CALL VstPlugin::processProc(vst2::AEffect* effect, float** /*inputs*/, float** outputs, int32_t frames) {
    static_cast<VstPlugin*>(effect->object)->process(outputs, frames, true);
}

void VST2_CALL VstPlugin::setParameterProc(vst2::AEffect* /*effect*/, int32_t /*index*/, float /*value*/) {}

float VST2_CALL VstPlugin::getParameterProc(vst2::AEffect* /*effect*/, int32_t /*index*/) {
    return 0.0f;
}

intptr_t VstPlugin::dispatch(int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt) {
    switch (opcode) {
    case vst2::effGetProgramName:
        if (ptr != nullptr) copyString(ptr, kProgramName, vst2::kVstMaxProgNameLen);
        return 0;
    case vst2::effGetProgramNameIndexed:
        if (ptr == nullptr || index != 0) return 0;
        copyString(ptr, kProgramName, vst2::kVstMaxProgNameLen);
        return 1;
    case vst2::effGetParamLabel:
    case vst2::effGetParamDisplay:
    case vst2::effGetParamName:
        if (ptr != nullptr) static_cast<char*>(ptr)[0] = '\0';
        return 0;
    case vst2::effSetSampleRate:
        core_.setSampleRate(double(opt));
        return 0;
    case vst2::effMainsChanged:
        if (value != 0) core_.activate();
        return 0;
    case vst2::effEditGetRect: {
        if (!editor_ || ptr == nullptr) return 0;
        int width = 0;
        int height = 0;
        editor_->size(width, height);
        editorRect_.top = 0;
        editorRect_.left = 0;
        editorRect_.bottom = int16_t(std::min(height, 32767));
        editorRect_.right = int16_t(std::min(width, 32767));
        *static_cast<vst2::ERect**>(ptr) = &editorRect_;
        return 1;
    }
    case vst2::effEditOpen:
        return editor_ && editor_->open(ptr) ? 1 : 0;
    case vst2::effEditClose:
        if (editor_) editor_->close();
        return 0;
    case vst2::effGetChunk: {
        if (ptr == nullptr) return 0;
        chunk_ = core_.saveState();
        *static_cast<void**>(ptr) = chunk_.data();
        return intptr_t(chunk_.size());
    }
    case vst2::effSetChunk:
        if (ptr == nullptr || value <= 0) return 0;
        return core_.loadState(static_cast<const uint8_t*>(ptr), size_t(value)) ? 1 : 0;
    case vst2::effProcessEvents:
        if (ptr != nullptr) queueEvents(*static_cast<const vst2::VstEvents*>(ptr));
        return 1;
    case vst2::effGetOutputProperties: {
        if (ptr == nullptr || index < 0 || index >= kOutputs) return 0;
        auto* properties = static_cast<vst2::VstPinProperties*>(ptr);
        if (index >= kPartPins && core_.multiOutputPairs()) {
            // "Multi 1+2 L" ("M1+2L") to "Multi 5+6 R" ("M5+6R"): the pairs, MULTI 1 on the left of the first.
            const int first = index - kPartPins - (index - kPartPins) % 2 + 1;
            const bool left = (index - kPartPins) % 2 == 0;
            const std::string pair = std::to_string(first) + "+" + std::to_string(first + 1);
            copyString(properties->label, ("Multi " + pair + (left ? " L" : " R")).c_str(), sizeof(properties->label));
            copyString(properties->shortLabel, ("M" + pair + (left ? "L" : "R")).c_str(), sizeof(properties->shortLabel));
            properties->flags = vst2::kVstPinIsActive | (left ? vst2::kVstPinIsStereo : 0);  // Stereo: this pin and the next
            properties->arrangementType = 1;  // Stereo
            return 1;
        }
        if (index >= kPartPins) {
            // "Multi 1" ("M1") to "Multi 6" ("M6"), mono.
            const std::string number = std::to_string(index - kPartPins + 1);
            copyString(properties->label, ("Multi " + number).c_str(), sizeof(properties->label));
            copyString(properties->shortLabel, ("M" + number).c_str(), sizeof(properties->shortLabel));
            properties->flags = vst2::kVstPinIsActive;
            properties->arrangementType = 0;  // Mono
            return 1;
        }
        const bool left = index % 2 == 0;
        const int part = index / 2 - 1;  // -1: the mix
        const std::string name = part < 0 ? std::string("D-110") : partOutputName(part);
        copyString(properties->label, (name + (left ? " L" : " R")).c_str(), sizeof(properties->label));
        // "L", "R" for the mix; "1L", "RR" (rhythm), "15R" for the parts.
        const std::string shortName = part < 0 ? std::string() : part == 8 ? std::string("R") : std::to_string(part < 8 ? part + 1 : part);
        copyString(properties->shortLabel, (shortName + (left ? "L" : "R")).c_str(), sizeof(properties->shortLabel));
        properties->flags = vst2::kVstPinIsActive | (left ? vst2::kVstPinIsStereo : 0);  // Stereo: this pin and the next
        properties->arrangementType = 1;  // Stereo
        return 1;
    }
    case vst2::effGetPlugCategory:
        return vst2::kPlugCategSynth;
    case vst2::effGetEffectName:
        if (ptr != nullptr) copyString(ptr, kEffectName, vst2::kVstMaxEffectNameLen);
        return 1;
    case vst2::effGetVendorString:
        if (ptr != nullptr) copyString(ptr, kVendor, vst2::kVstMaxVendorStrLen);
        return 1;
    case vst2::effGetProductString:
        if (ptr != nullptr) copyString(ptr, kProduct, vst2::kVstMaxProductStrLen);
        return 1;
    case vst2::effGetVendorVersion:
        return kVendorVersion;
    case vst2::effCanDo:
        return canDo(static_cast<const char*>(ptr));
    case vst2::effGetVstVersion:
        return vst2::kVstVersion;
    case vst2::effEditKeyDown:
    case vst2::effEditKeyUp: {
        if (!editor_) return 0;
        const int modifiers = int(opt);
        return editor_->key(index, int(value), (modifiers & vst2::MODIFIER_SHIFT) != 0, (modifiers & vst2::MODIFIER_CONTROL) != 0,
                            (modifiers & vst2::MODIFIER_ALTERNATE) != 0, opcode == vst2::effEditKeyDown)
                   ? 1
                   : 0;
    }
    case vst2::effGetNumMidiInputChannels:
        return 16;
    default:
        // effOpen, effSetProgram (one program: the synth's memory), effSetBlockSize (blocks of any length are fine),
        // effEditIdle (the editor draws on its own timer), effGetTailSize, parameters (there are none) and the rest.
        return 0;
    }
}

void VstPlugin::queueEvents(const vst2::VstEvents& events) {
    const vst2::VstEvent* const* list = events.events;  // numEvents long, whatever the declaration says
    for (int32_t i = 0; i < events.numEvents; i++) {
        const vst2::VstEvent* event = list[i];
        if (event == nullptr) continue;
        const uint32_t frame = uint32_t(std::max<int32_t>(event->deltaFrames, 0));
        if (event->type == vst2::kVstMidiType) {
            const auto* midi = reinterpret_cast<const vst2::VstMidiEvent*>(event);
            core_.queueMidi(frame, uint8_t(midi->midiData[0]), uint8_t(midi->midiData[1]), uint8_t(midi->midiData[2]));
        } else if (event->type == vst2::kVstSysExType) {
            const auto* message = reinterpret_cast<const vst2::VstMidiSysexEvent*>(event);
            if (message->sysexDump == nullptr || message->dumpBytes < 2) continue;
            core_.queueSysex(frame, reinterpret_cast<const uint8_t*>(message->sysexDump), size_t(message->dumpBytes));
        }
    }
}

void VstPlugin::process(float** outputs, int32_t frames, bool accumulate) {
    if (frames <= 0 || outputs == nullptr) return;
    float* partLeft[SynthEngine::kPartOutputs];
    float* partRight[SynthEngine::kPartOutputs];
    for (int part = 0; part < SynthEngine::kPartOutputs; part++) {
        partLeft[part] = outputs[2 + 2 * part];
        partRight[part] = outputs[3 + 2 * part];
    }
    core_.render(outputs[0], outputs[1], partLeft, partRight, outputs + kPartPins, uint32_t(frames), accumulate);
}

// ---------------------------------------------------------------------------------------------
// The entry point: hosts look for VSTPluginMain (and old ones for "main", which D110EmuVST.def adds on Windows).

#if defined(_WIN32)
#define D110EMU_EXPORT __declspec(dllexport)
#else
#define D110EMU_EXPORT __attribute__((visibility("default")))
#endif

extern "C" D110EMU_EXPORT vst2::AEffect* VSTPluginMain(vst2::HostCallback host) {
    if (host == nullptr) return nullptr;
    VstPlugin* plugin = new (std::nothrow) VstPlugin(host);
    return plugin != nullptr ? plugin->effect() : nullptr;
}
