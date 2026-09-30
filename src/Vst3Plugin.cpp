// D110Emu as a VST3 instrument: PluginCore (the App, as the VST2 plugin runs it) behind the VST3 interfaces, from
// Steinberg's MIT-licensed interface headers (vendor/vst3sdk). The mix plays out of the main output, and each part the
// App's Output menus put on its own output out of an output of its own, with its pan (its reverb stays in the mix).
//
// The processor (the audio: the component) and the controller (the editor, the MIDI mapping) are separate objects, as
// VST3 hosts expect; they share one Instance (the core and its editor), which the processor makes and names to the
// controller through their connection. Both must run in one process, as every host runs them.
//
// VST3 carries no MIDI as such. Notes come as note events, SysEx as data events (in the hosts that send them), and the
// controllers, pitch bend, channel pressure and program changes as changes of parameters the controller maps them to
// (IMidiMapping); program changes also as a unit's program per MIDI channel (IUnitInfo). The processor turns them all
// back into MIDI.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "PluginCore.h"
#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/keycodes.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstunits.h"
#include "pluginterfaces/vst/vstspeaker.h"

// The editor's window: an HWND on Windows, an NSView on macOS, an X11 window elsewhere (Linux), run by the host's event
// loop.
#if !defined(_WIN32) && !defined(__APPLE__)
#define D110EMU_X11_EDITOR 1
#endif

// The interface IDs of the VST interfaces used here (the SDK defines them in public.sdk, which is not vendored).
namespace Steinberg {
DEF_CLASS_IID (Vst::IComponent)
DEF_CLASS_IID (Vst::IAudioProcessor)
DEF_CLASS_IID (Vst::IEditController)
DEF_CLASS_IID (Vst::IMidiMapping)
DEF_CLASS_IID (Vst::IUnitInfo)
DEF_CLASS_IID (Vst::IConnectionPoint)
DEF_CLASS_IID (Vst::IComponentHandler)
DEF_CLASS_IID (Vst::IParamValueQueue)
DEF_CLASS_IID (Vst::IParameterChanges)
DEF_CLASS_IID (Vst::IEventList)
DEF_CLASS_IID (Vst::IMessage)
DEF_CLASS_IID (Vst::IHostApplication)
DEF_CLASS_IID (Vst::IAttributeList)
DEF_CLASS_IID (IPlugView)
DEF_CLASS_IID (IPlugFrame)
DEF_CLASS_IID (IPlugViewContentScaleSupport)
#ifdef D110EMU_X11_EDITOR
DEF_CLASS_IID (Linux::IRunLoop)
DEF_CLASS_IID (Linux::IEventHandler)
DEF_CLASS_IID (Linux::ITimerHandler)
#endif
}  // namespace Steinberg

using namespace Steinberg;

namespace {

const TUID kProcessorCid = INLINE_UID(0xFAEBAF66, 0xBCD747D2, 0x98F23412, 0xE65806F4);
const TUID kControllerCid = INLINE_UID(0x6D502CA9, 0x1ECE4A1C, 0x8DF6E081, 0x611F0CAD);
const char* const kName = "D110Emu";
const char* const kVendor = "D110Emu";
const char* const kVersion = "1.0.0";
const char* const kInstanceMessage = "D110EmuInstance";  // The processor names its Instance to the controller
const char* const kInstanceAttribute = "instance";

constexpr int kPartBuses = SynthEngine::kPartOutputs;    // Outputs 1-16: parts 1-8, rhythm, parts 9-15 (stereo)
// Then MULTI 1-6: outputs 17-22, mono; or with pairs (PluginCore::multiOutputPairs) outputs 17-19, stereo: 1+2, 3+4, 5+6.
constexpr int kChannels = 16;
// The controller's parameters: for each MIDI channel, the controllers 0-127, channel pressure (128), pitch bend (129)
// and the program change (130), which IMidiMapping hands the host for them.
constexpr int kMidiControls = 131;
constexpr int kProgramControl = Vst::kCtrlProgramChange;  // 130
constexpr Vst::ParamID kMidiParamBase = 0x10000;
constexpr int kParameterCount = kChannels * kMidiControls;
constexpr Vst::ProgramListID kProgramList = 1;  // The timbre memory, A11-B88: every channel's unit uses it

void setString(Vst::String128 out, const std::string& ascii) {
    size_t i = 0;
    for (; i < ascii.size() && i < 127; i++) out[i] = char16(static_cast<unsigned char>(ascii[i]));
    out[i] = 0;
}

void setString16(char16* out, size_t size, const std::string& ascii) {
    size_t i = 0;
    for (; i < ascii.size() && i + 1 < size; i++) out[i] = char16(static_cast<unsigned char>(ascii[i]));
    out[i] = 0;
}

std::string partBusName(int part) {
    if (part == 8) return "Rhythm";
    return "Part " + std::to_string(part < 8 ? part + 1 : part);  // Parts 9-15 are part numbers 9-15
}

std::string programName(int program) {  // Timbre memory, as the unit numbers it: I-A11 ... I-B88
    return std::string("I-") + char('A' + program / 64) + char('1' + program % 64 / 8) + char('1' + program % 8);
}

// ---------------------------------------------------------------------------------------------
// The Instance: the core and its editor, shared by the processor, the controller and the view.

struct Instance {
    Instance() : core(PluginFormat::Vst3), editor(createPluginEditor(core)) {
        if (editor) core.setEditor(editor.get());
    }
    ~Instance() {
        core.setEditor(nullptr);
        editor.reset();  // Closes the window before the core goes
    }
    PluginCore core;
    std::unique_ptr<PluginEditor> editor;
};

// Instances by number, so that the controller finds its processor's in this process (a number from elsewhere names none).
std::mutex& registryMutex() {
    static std::mutex mutex;
    return mutex;
}

std::map<int64, std::weak_ptr<Instance>>& registry() {
    static std::map<int64, std::weak_ptr<Instance>> instances;
    return instances;
}

int64 registerInstance(const std::shared_ptr<Instance>& instance) {
    static int64 next = 1;
    std::lock_guard<std::mutex> lock(registryMutex());
    const int64 id = next++;
    registry()[id] = instance;
    return id;
}

void unregisterInstance(int64 id) {
    std::lock_guard<std::mutex> lock(registryMutex());
    registry().erase(id);
}

std::shared_ptr<Instance> findInstance(int64 id) {
    std::lock_guard<std::mutex> lock(registryMutex());
    const auto found = registry().find(id);
    return found != registry().end() ? found->second.lock() : nullptr;
}

// A message of our own, for hosts that make none (IHostApplication::createInstance): the Instance's number.
class InstanceAttributes : public U::Implements<U::Directly<Vst::IAttributeList>> {
public:
    tresult PLUGIN_API setInt(AttrID id, int64 value) override {
        if (id == nullptr || std::strcmp(id, kInstanceAttribute) != 0) return kResultFalse;
        value_ = value;
        return kResultOk;
    }
    tresult PLUGIN_API getInt(AttrID id, int64& value) override {
        if (id == nullptr || std::strcmp(id, kInstanceAttribute) != 0) return kResultFalse;
        value = value_;
        return kResultOk;
    }
    tresult PLUGIN_API setFloat(AttrID, double) override { return kResultFalse; }
    tresult PLUGIN_API getFloat(AttrID, double&) override { return kResultFalse; }
    tresult PLUGIN_API setString(AttrID, const Vst::TChar*) override { return kResultFalse; }
    tresult PLUGIN_API getString(AttrID, Vst::TChar*, uint32) override { return kResultFalse; }
    tresult PLUGIN_API setBinary(AttrID, const void*, uint32) override { return kResultFalse; }
    tresult PLUGIN_API getBinary(AttrID, const void*&, uint32&) override { return kResultFalse; }

private:
    int64 value_ = 0;
};

class InstanceMessage : public U::Implements<U::Directly<Vst::IMessage>> {
public:
    InstanceMessage() : attributes_(new InstanceAttributes) {}
    ~InstanceMessage() override { attributes_->release(); }
    FIDString PLUGIN_API getMessageID() override { return id_.c_str(); }
    void PLUGIN_API setMessageID(FIDString id) override { id_ = id != nullptr ? id : ""; }
    Vst::IAttributeList* PLUGIN_API getAttributes() override { return attributes_; }

private:
    std::string id_;
    InstanceAttributes* attributes_;
};

// ---------------------------------------------------------------------------------------------
// The processor

class Processor : public U::Implements<U::Directly<Vst::IComponent, Vst::IAudioProcessor, Vst::IConnectionPoint>, U::Indirectly<IPluginBase>> {
public:
    ~Processor() override { terminate(); }

    // IPluginBase
    tresult PLUGIN_API initialize(FUnknown* context) override {
        if (instance_) return kResultFalse;
        context_ = context;
        instance_ = std::make_shared<Instance>();
        instanceId_ = registerInstance(instance_);
        multiPairs_ = instance_->core.multiOutputPairs();
        return kResultOk;
    }
    tresult PLUGIN_API terminate() override {
        if (peer_ != nullptr) disconnect(peer_);
        if (instanceId_ != 0) unregisterInstance(instanceId_);
        instanceId_ = 0;
        instance_.reset();
        context_ = nullptr;
        return kResultOk;
    }

    // IComponent
    tresult PLUGIN_API getControllerClassId(TUID classId) override {
        std::memcpy(classId, kControllerCid, sizeof(TUID));
        return kResultOk;
    }
    tresult PLUGIN_API setIoMode(Vst::IoMode) override { return kResultOk; }
    int32 PLUGIN_API getBusCount(Vst::MediaType type, Vst::BusDirection dir) override {
        if (type == Vst::kAudio && dir == Vst::kOutput) return outputBuses();
        if (type == Vst::kEvent && dir == Vst::kInput) return 1;
        return 0;
    }
    tresult PLUGIN_API getBusInfo(Vst::MediaType type, Vst::BusDirection dir, int32 index, Vst::BusInfo& bus) override {
        if (type == Vst::kAudio && dir == Vst::kOutput && index >= 0 && index < outputBuses()) {
            bus.mediaType = Vst::kAudio;
            bus.direction = Vst::kOutput;
            const bool multi = index > kPartBuses;
            bus.channelCount = channelsOf(index);
            const int first = multiPairs_ ? 2 * (index - kPartBuses) - 1 : index - kPartBuses;  // Its (first) MULTI output
            setString(bus.name, index == 0    ? std::string("D-110 Mix")
                                : !multi      ? partBusName(index - 1)
                                : multiPairs_ ? "Multi " + std::to_string(first) + "+" + std::to_string(first + 1)
                                              : "Multi " + std::to_string(first));
            bus.busType = index == 0 ? Vst::kMain : Vst::kAux;
            // All active from the start: some hosts (Cakewalk) list only the outputs a plugin activates by default.
            bus.flags = Vst::BusInfo::kDefaultActive;
            return kResultOk;
        }
        if (type == Vst::kEvent && dir == Vst::kInput && index == 0) {
            bus.mediaType = Vst::kEvent;
            bus.direction = Vst::kInput;
            bus.channelCount = kChannels;
            setString(bus.name, "MIDI In");
            bus.busType = Vst::kMain;
            bus.flags = Vst::BusInfo::kDefaultActive;
            return kResultOk;
        }
        return kInvalidArgument;
    }
    tresult PLUGIN_API getRoutingInfo(Vst::RoutingInfo&, Vst::RoutingInfo&) override { return kResultFalse; }
    tresult PLUGIN_API activateBus(Vst::MediaType type, Vst::BusDirection dir, int32 index, TBool state) override {
        if (type != Vst::kAudio || dir != Vst::kOutput || index < 0 || index >= outputBuses()) return kResultOk;
        if (index == 0 || !instance_) return kResultOk;
        // Only shown: the parts the App's Output menus put on their own outputs (or MULTI outputs) play there whatever
        // the host takes, and one on an output the host has off is pointed out. Bit n: part number n's own output; bit
        // 16 + n: MULTI n + 1 (both of a pair's).
        const int multi = index - 1 - kPartBuses;  // 0-based among the MULTI outputs
        const uint32_t bits = index <= kPartBuses ? 1u << (index - 1) : multiPairs_ ? 3u << (16 + 2 * multi) : 1u << (16 + multi);
        outputMask_ = state ? outputMask_ | bits : outputMask_ & ~bits;
        instance_->core.setHostOutputs(outputMask_);
        return kResultOk;
    }
    tresult PLUGIN_API setActive(TBool state) override {
        if (state && instance_) instance_->core.activate();
        return kResultOk;
    }
    tresult PLUGIN_API setState(IBStream* state) override {
        if (state == nullptr || !instance_) return kInvalidArgument;
        std::vector<uint8_t> data;
        std::vector<uint8_t> chunk(65536);
        for (;;) {
            int32 read = 0;
            if (state->read(chunk.data(), int32(chunk.size()), &read) != kResultOk || read <= 0) break;
            data.insert(data.end(), chunk.begin(), chunk.begin() + read);
            if (read < int32(chunk.size())) break;
        }
        return instance_->core.loadState(data.data(), data.size()) ? kResultOk : kResultFalse;
    }
    tresult PLUGIN_API getState(IBStream* state) override {
        if (state == nullptr || !instance_) return kInvalidArgument;
        std::vector<uint8_t> data = instance_->core.saveState();
        size_t written = 0;
        while (written < data.size()) {
            int32 count = 0;
            const int32 wanted = int32(std::min<size_t>(data.size() - written, 1 << 20));
            if (state->write(data.data() + written, wanted, &count) != kResultOk || count <= 0) return kResultFalse;
            written += size_t(count);
        }
        return kResultOk;
    }

    // IAudioProcessor
    tresult PLUGIN_API setBusArrangements(Vst::SpeakerArrangement*, int32 numIns, Vst::SpeakerArrangement* outputs, int32 numOuts) override {
        if (numIns != 0 || numOuts < 1 || numOuts > outputBuses() || outputs == nullptr) return kResultFalse;
        for (int32 i = 0; i < numOuts; i++) {
            // Ours only (stereo; the MULTI outputs mono, unless they are pairs); the host keeps asking them.
            if (outputs[i] != busArrangement(i)) return kResultFalse;
        }
        return kResultTrue;
    }
    tresult PLUGIN_API getBusArrangement(Vst::BusDirection dir, int32 index, Vst::SpeakerArrangement& arrangement) override {
        if (dir != Vst::kOutput || index < 0 || index >= outputBuses()) return kInvalidArgument;
        arrangement = busArrangement(index);
        return kResultOk;
    }
    tresult PLUGIN_API canProcessSampleSize(int32 symbolicSampleSize) override {
        return symbolicSampleSize == Vst::kSample32 ? kResultTrue : kResultFalse;
    }
    uint32 PLUGIN_API getLatencySamples() override { return 0; }
    tresult PLUGIN_API setupProcessing(Vst::ProcessSetup& setup) override {
        if (setup.symbolicSampleSize != Vst::kSample32) return kResultFalse;
        if (instance_) instance_->core.setSampleRate(setup.sampleRate);
        return kResultOk;
    }
    tresult PLUGIN_API setProcessing(TBool) override { return kResultOk; }
    tresult PLUGIN_API process(Vst::ProcessData& data) override;
    uint32 PLUGIN_API getTailSamples() override { return Vst::kInfiniteTail; }

    // IConnectionPoint
    tresult PLUGIN_API connect(Vst::IConnectionPoint* other) override;
    tresult PLUGIN_API disconnect(Vst::IConnectionPoint* other) override {
        if (other == nullptr || other != peer_) return kResultFalse;
        peer_->release();
        peer_ = nullptr;
        return kResultOk;
    }
    tresult PLUGIN_API notify(Vst::IMessage*) override { return kResultOk; }

private:
    int outputBuses() const { return 1 + kPartBuses + (multiPairs_ ? SynthEngine::kMultiPairs : PluginCore::kMultiOutputs); }
    int32 channelsOf(int32 bus) const { return bus > kPartBuses && !multiPairs_ ? 1 : 2; }
    Vst::SpeakerArrangement busArrangement(int32 index) const {
        return channelsOf(index) == 1 ? Vst::SpeakerArr::kMono : Vst::SpeakerArr::kStereo;
    }
    void queueEvent(const Vst::Event& event, bool sysex);
    void queueParameterChange(Vst::ParamID id, int32 offset, Vst::ParamValue value);

    FUnknown* context_ = nullptr;
    std::shared_ptr<Instance> instance_;
    int64 instanceId_ = 0;
    Vst::IConnectionPoint* peer_ = nullptr;
    uint32_t outputMask_ = 0;  // The outputs the host takes, as PluginCore::setHostOutputs has them
    bool multiPairs_ = false;  // MULTI 1-6 as three stereo buses (the instance's layout, settled by initialize)
};

tresult PLUGIN_API Processor::connect(Vst::IConnectionPoint* other) {
    if (other == nullptr) return kInvalidArgument;
    if (peer_ != nullptr) return kResultFalse;
    peer_ = other;
    peer_->addRef();
    // Name the Instance to the controller: in a message the host makes, else in one of ours.
    Vst::IMessage* message = nullptr;
    if (context_ != nullptr) {
        Vst::IHostApplication* host = nullptr;
        if (context_->queryInterface(Vst::IHostApplication::iid, reinterpret_cast<void**>(&host)) == kResultOk && host != nullptr) {
            TUID cid;
            std::memcpy(cid, Vst::IMessage::iid, sizeof(TUID));
            TUID iid;
            std::memcpy(iid, Vst::IMessage::iid, sizeof(TUID));
            if (host->createInstance(cid, iid, reinterpret_cast<void**>(&message)) != kResultOk) message = nullptr;
            host->release();
        }
    }
    if (message == nullptr) message = new InstanceMessage;
    message->setMessageID(kInstanceMessage);
    if (message->getAttributes() != nullptr) message->getAttributes()->setInt(kInstanceAttribute, instanceId_);
    peer_->notify(message);
    message->release();
    return kResultOk;
}

void Processor::queueEvent(const Vst::Event& event, bool sysex) {
    const uint32_t frame = uint32_t(std::max<int32>(event.sampleOffset, 0));
    PluginCore& core = instance_->core;
    const auto seven = [](float value) { return uint8_t(std::clamp(int(std::lround(value * 127.0f)), 0, 127)); };
    switch (event.type) {
    case Vst::Event::kDataEvent:
        if (sysex && event.data.type == Vst::DataEvent::kMidiSysEx && event.data.bytes != nullptr) {
            core.queueSysex(frame, event.data.bytes, event.data.size);
        }
        break;
    case Vst::Event::kNoteOnEvent:
        if (!sysex) {
            uint8_t velocity = seven(event.noteOn.velocity);
            if (velocity == 0 && event.noteOn.velocity > 0.0f) velocity = 1;  // A soft note, not a note-off
            core.queueMidi(frame, uint8_t(0x90 | (event.noteOn.channel & 0x0F)), uint8_t(event.noteOn.pitch & 0x7F), velocity);
        }
        break;
    case Vst::Event::kNoteOffEvent:
        if (!sysex) {
            core.queueMidi(frame, uint8_t(0x80 | (event.noteOff.channel & 0x0F)), uint8_t(event.noteOff.pitch & 0x7F),
                           seven(event.noteOff.velocity));
        }
        break;
    case Vst::Event::kPolyPressureEvent:
        if (!sysex) {
            core.queueMidi(frame, uint8_t(0xA0 | (event.polyPressure.channel & 0x0F)), uint8_t(event.polyPressure.pitch & 0x7F),
                           seven(event.polyPressure.pressure));
        }
        break;
    case Vst::Event::kLegacyMIDICCOutEvent: {
        // Some hosts send MIDI this way too.
        if (sysex) break;
        const Vst::LegacyMIDICCOutEvent& cc = event.midiCCOut;
        const uint8_t channel = uint8_t(cc.channel & 0x0F);
        const uint8_t value = uint8_t(cc.value & 0x7F);
        const uint8_t value2 = uint8_t(cc.value2 & 0x7F);
        if (cc.controlNumber < 128) {
            core.queueMidi(frame, uint8_t(0xB0 | channel), cc.controlNumber, value);
        } else if (cc.controlNumber == Vst::kAfterTouch) {
            core.queueMidi(frame, uint8_t(0xD0 | channel), value, 0);
        } else if (cc.controlNumber == Vst::kPitchBend) {
            core.queueMidi(frame, uint8_t(0xE0 | channel), value, value2);
        } else if (cc.controlNumber == Vst::kCtrlProgramChange) {
            core.queueMidi(frame, uint8_t(0xC0 | channel), value, 0);
        } else if (cc.controlNumber == Vst::kCtrlPolyPressure) {
            core.queueMidi(frame, uint8_t(0xA0 | channel), value, value2);
        }
        break;
    }
    default:
        break;
    }
}

void Processor::queueParameterChange(Vst::ParamID id, int32 offset, Vst::ParamValue value) {
    if (id < kMidiParamBase || id >= kMidiParamBase + Vst::ParamID(kParameterCount)) return;
    const int index = int(id - kMidiParamBase);
    const uint8_t channel = uint8_t(index / kMidiControls);
    const int control = index % kMidiControls;
    const uint32_t frame = uint32_t(std::max<int32>(offset, 0));
    value = std::clamp(value, 0.0, 1.0);
    PluginCore& core = instance_->core;
    const uint8_t seven = uint8_t(std::lround(value * 127.0));
    if (control < 128) {
        core.queueMidi(frame, uint8_t(0xB0 | channel), uint8_t(control), seven);
    } else if (control == Vst::kAfterTouch) {
        core.queueMidi(frame, uint8_t(0xD0 | channel), seven, 0);
    } else if (control == Vst::kPitchBend) {
        const int bend = int(std::lround(value * 16383.0));
        core.queueMidi(frame, uint8_t(0xE0 | channel), uint8_t(bend & 0x7F), uint8_t(bend >> 7));
    } else if (control == kProgramControl) {
        core.queueMidi(frame, uint8_t(0xC0 | channel), seven, 0);
    }
}

tresult PLUGIN_API Processor::process(Vst::ProcessData& data) {
    if (!instance_) return kResultFalse;
    if (data.symbolicSampleSize != Vst::kSample32) return kResultFalse;
    // At the same frame, as a MIDI file usually has them: SysEx first, then controllers and program changes, then notes.
    if (data.inputEvents != nullptr) {
        const int32 count = data.inputEvents->getEventCount();
        for (int32 i = 0; i < count; i++) {
            Vst::Event event = {};
            if (data.inputEvents->getEvent(i, event) == kResultOk) queueEvent(event, true);
        }
    }
    if (data.inputParameterChanges != nullptr) {
        const int32 count = data.inputParameterChanges->getParameterCount();
        for (int32 i = 0; i < count; i++) {
            Vst::IParamValueQueue* queue = data.inputParameterChanges->getParameterData(i);
            if (queue == nullptr) continue;
            const Vst::ParamID id = queue->getParameterId();
            const int32 points = queue->getPointCount();
            for (int32 point = 0; point < points; point++) {
                int32 offset = 0;
                Vst::ParamValue value = 0.0;
                if (queue->getPoint(point, offset, value) == kResultOk) queueParameterChange(id, offset, value);
            }
        }
    }
    if (data.inputEvents != nullptr) {
        const int32 count = data.inputEvents->getEventCount();
        for (int32 i = 0; i < count; i++) {
            Vst::Event event = {};
            if (data.inputEvents->getEvent(i, event) == kResultOk) queueEvent(event, false);
        }
    }
    if (data.numSamples <= 0 || data.numOutputs <= 0 || data.outputs == nullptr) return kResultOk;  // The MIDI waits
    // A bus's channels, where it has as many as ours (a host keeps to our arrangements, but just in case).
    const auto channel = [&](int32 bus, int32 index) -> float* {
        if (bus >= data.numOutputs || bus >= outputBuses()) return nullptr;
        const Vst::AudioBusBuffers& buffers = data.outputs[bus];
        if (buffers.numChannels < channelsOf(bus) || buffers.channelBuffers32 == nullptr) return nullptr;
        return buffers.channelBuffers32[index];
    };
    float* partLeft[kPartBuses] = {};
    float* partRight[kPartBuses] = {};
    float* multi[PluginCore::kMultiOutputs] = {};
    for (int part = 0; part < kPartBuses; part++) {
        partLeft[part] = channel(1 + part, 0);
        partRight[part] = channel(1 + part, 1);
    }
    for (int output = 0; output < PluginCore::kMultiOutputs; output++) {
        // Pairs: MULTI 1 and 2 on the left and right of the first MULTI bus, and so on.
        multi[output] = multiPairs_ ? channel(1 + kPartBuses + output / 2, output % 2) : channel(1 + kPartBuses + output, 0);
    }
    instance_->core.render(channel(0, 0), channel(0, 1), partLeft, partRight, multi, uint32_t(data.numSamples), false);
    for (int32 bus = 0; bus < data.numOutputs; bus++) {
        Vst::AudioBusBuffers& buffers = data.outputs[bus];
        buffers.silenceFlags = 0;
        // Channels render() did not fill (a bus of another width, or one past ours) are silent.
        const int32 filled = channel(bus, 0) == nullptr ? 0 : channelsOf(bus);
        for (int32 c = filled; c < buffers.numChannels && buffers.channelBuffers32 != nullptr; c++) {
            if (buffers.channelBuffers32[c] != nullptr) std::fill(buffers.channelBuffers32[c], buffers.channelBuffers32[c] + data.numSamples, 0.0f);
        }
    }
    return kResultOk;
}

// ---------------------------------------------------------------------------------------------
// The view: the editor window in the host's

// The host's context from the factory (IPluginFactory3::setHostContext), referenced: some Linux hosts give their run
// loop there rather than through the view's frame. The host's UI thread uses it.
FUnknown* g_hostContext = nullptr;

void releaseHostContext() {
    if (g_hostContext != nullptr) g_hostContext->release();
    g_hostContext = nullptr;
}

#ifdef D110EMU_X11_EDITOR
// The editor's part in the host's event loop (VST3's Linux::IRunLoop): the events of its X connection, and a frame
// every 16 ms. The host may keep a reference to it after the view is gone: it reaches the editor only until detach().
class RunLoopClient : public U::Implements<U::Directly<Linux::IEventHandler, Linux::ITimerHandler>> {
public:
    explicit RunLoopClient(PluginEditor* editor) : editor_(editor) {}
    void PLUGIN_API onFDIsSet(Linux::FileDescriptor) override {
        if (editor_ != nullptr) editor_->processEvents();
    }
    void PLUGIN_API onTimer() override {
        if (editor_ != nullptr) editor_->idle();
    }
    void detach() { editor_ = nullptr; }

private:
    PluginEditor* editor_;
};
#endif

class View : public U::Implements<U::Directly<IPlugView, IPlugViewContentScaleSupport>> {
public:
    explicit View(std::shared_ptr<Instance> instance) : instance_(std::move(instance)) {}
    ~View() override { removed(); }

    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override {
#if defined(_WIN32)
        if (type != nullptr && std::strcmp(type, kPlatformTypeHWND) == 0 && instance_->editor) return kResultTrue;
#elif defined(__APPLE__)
        if (type != nullptr && std::strcmp(type, kPlatformTypeNSView) == 0 && instance_->editor) return kResultTrue;
#elif defined(D110EMU_X11_EDITOR)
        if (type != nullptr && std::strcmp(type, kPlatformTypeX11EmbedWindowID) == 0 && instance_->editor) return kResultTrue;
#else
        (void)type;
#endif
        return kResultFalse;
    }
    tresult PLUGIN_API attached(void* parent, FIDString type) override {
        if (parent == nullptr || isPlatformTypeSupported(type) != kResultTrue) return kResultFalse;
        PluginEditor& editor = *instance_->editor;
#ifdef D110EMU_X11_EDITOR
        // The window runs on the host's event loop, which the frame gives (or, in some hosts, the factory's context).
        Linux::IRunLoop* runLoop = nullptr;
        if (frame_ != nullptr) frame_->queryInterface(Linux::IRunLoop::iid, reinterpret_cast<void**>(&runLoop));
        if (runLoop == nullptr && g_hostContext != nullptr) g_hostContext->queryInterface(Linux::IRunLoop::iid, reinterpret_cast<void**>(&runLoop));
        if (runLoop == nullptr) return kResultFalse;
#endif
        editor.setHostResize([this](int width, int height) {
            if (frame_ == nullptr) return false;
            ViewRect rect(0, 0, width, height);
            return frame_->resizeView(this, &rect) == kResultTrue;
        });
        if (!editor.open(parent)) {
            editor.setHostResize(nullptr);
#ifdef D110EMU_X11_EDITOR
            runLoop->release();
#endif
            return kResultFalse;
        }
#ifdef D110EMU_X11_EDITOR
        runLoop_ = runLoop;
        runLoopClient_ = new RunLoopClient(&editor);
        // (The frames on the timer take the window's events too, so the window runs even where the host refuses the descriptor.)
        if (editor.eventDescriptor() >= 0) runLoop_->registerEventHandler(static_cast<Linux::IEventHandler*>(runLoopClient_), editor.eventDescriptor());
        runLoop_->registerTimer(static_cast<Linux::ITimerHandler*>(runLoopClient_), 16);  // About 60 frames a second
#endif
        open_ = true;
        return kResultOk;
    }
    tresult PLUGIN_API removed() override {
        if (!open_) return kResultOk;
        open_ = false;
#ifdef D110EMU_X11_EDITOR
        runLoop_->unregisterEventHandler(static_cast<Linux::IEventHandler*>(runLoopClient_));
        runLoop_->unregisterTimer(static_cast<Linux::ITimerHandler*>(runLoopClient_));
        runLoop_->release();
        runLoop_ = nullptr;
        runLoopClient_->detach();
        runLoopClient_->release();
        runLoopClient_ = nullptr;
#endif
        instance_->editor->close();
        instance_->editor->setHostResize(nullptr);
        return kResultOk;
    }
    tresult PLUGIN_API onWheel(float) override { return kResultFalse; }
    tresult PLUGIN_API onKeyDown(char16 key, int16 keyCode, int16 modifiers) override { return onKey(key, keyCode, modifiers, true); }
    tresult PLUGIN_API onKeyUp(char16 key, int16 keyCode, int16 modifiers) override { return onKey(key, keyCode, modifiers, false); }
    tresult PLUGIN_API getSize(ViewRect* size) override {
        if (size == nullptr || !instance_->editor) return kInvalidArgument;
        int width = 0;
        int height = 0;
        instance_->editor->size(width, height);
        *size = ViewRect(0, 0, width, height);
        return kResultOk;
    }
    tresult PLUGIN_API onSize(ViewRect* newSize) override {
        if (newSize == nullptr || !instance_->editor) return kInvalidArgument;
        instance_->editor->setSize(newSize->getWidth(), newSize->getHeight());
        return kResultOk;
    }
    tresult PLUGIN_API onFocus(TBool) override { return kResultOk; }
    tresult PLUGIN_API setFrame(IPlugFrame* frame) override {
        frame_ = frame;
        return kResultOk;
    }
    tresult PLUGIN_API canResize() override { return kResultFalse; }  // View > Window size sets it
    tresult PLUGIN_API checkSizeConstraint(ViewRect* rect) override {
        if (rect == nullptr || !instance_->editor) return kInvalidArgument;
        int width = 0;
        int height = 0;
        instance_->editor->size(width, height);
        rect->right = rect->left + width;
        rect->bottom = rect->top + height;
        return kResultTrue;
    }
    tresult PLUGIN_API setContentScaleFactor(ScaleFactor factor) override {
        if (instance_->editor) instance_->editor->setScale(factor);
        return kResultTrue;
    }

private:
    tresult onKey(char16 key, int16 keyCode, int16 modifiers, bool down) {
        if (!open_) return kResultFalse;
        const bool used = instance_->editor->key(int(key), int(keyCode), (modifiers & kShiftKey) != 0, (modifiers & kCommandKey) != 0,
                                                 (modifiers & kAlternateKey) != 0, down);
        return used ? kResultTrue : kResultFalse;
    }

    std::shared_ptr<Instance> instance_;
    IPlugFrame* frame_ = nullptr;
    bool open_ = false;
#ifdef D110EMU_X11_EDITOR
    Linux::IRunLoop* runLoop_ = nullptr;  // Referenced while the window is open
    RunLoopClient* runLoopClient_ = nullptr;
#endif
};

// ---------------------------------------------------------------------------------------------
// The controller

class Controller
    : public U::Implements<U::Directly<Vst::IEditController, Vst::IMidiMapping, Vst::IUnitInfo, Vst::IConnectionPoint>, U::Indirectly<IPluginBase>> {
public:
    Controller() {
        values_.assign(kParameterCount, 0.0);
        for (int channel = 0; channel < kChannels; channel++) {
            for (int control = 0; control < kMidiControls; control++) values_[size_t(channel * kMidiControls + control)] = defaultValue(control);
        }
    }
    ~Controller() override { terminate(); }

    // IPluginBase
    tresult PLUGIN_API initialize(FUnknown*) override { return kResultOk; }
    tresult PLUGIN_API terminate() override {
        if (peer_ != nullptr) disconnect(peer_);
        if (handler_ != nullptr) handler_->release();
        handler_ = nullptr;
        instance_.reset();
        return kResultOk;
    }

    // IEditController: the state lives in the processor's (the core's), so the controller has none of its own.
    tresult PLUGIN_API setComponentState(IBStream*) override { return kResultOk; }
    tresult PLUGIN_API setState(IBStream*) override { return kResultOk; }
    tresult PLUGIN_API getState(IBStream*) override { return kResultOk; }
    int32 PLUGIN_API getParameterCount() override { return kParameterCount; }
    tresult PLUGIN_API getParameterInfo(int32 index, Vst::ParameterInfo& info) override {
        if (index < 0 || index >= kParameterCount) return kInvalidArgument;
        const int channel = index / kMidiControls;
        const int control = index % kMidiControls;
        std::memset(&info, 0, sizeof(info));
        info.id = kMidiParamBase + Vst::ParamID(index);
        const std::string prefix = "Ch " + std::to_string(channel + 1) + " ";
        std::string name;
        if (control < 128) {
            name = "CC " + std::to_string(control);
        } else if (control == Vst::kAfterTouch) {
            name = "Pressure";
        } else if (control == Vst::kPitchBend) {
            name = "Pitch bend";
        } else {
            name = "Program";
        }
        setString(info.title, "MIDI " + prefix + name);
        setString(info.shortTitle, prefix + name);
        setString(info.units, "");
        info.stepCount = control == Vst::kPitchBend ? 16383 : 127;
        info.defaultNormalizedValue = defaultValue(control);
        // The program change is the unit's program (one unit per MIDI channel); the controllers are there for the
        // host's MIDI mapping only, as JUCE's VST3 plugins have them.
        info.unitId = control == kProgramControl ? Vst::UnitID(channel + 1) : Vst::kRootUnitId;
        info.flags = control == kProgramControl ? Vst::ParameterInfo::kIsProgramChange | Vst::ParameterInfo::kIsList : Vst::ParameterInfo::kNoFlags;
        return kResultOk;
    }
    tresult PLUGIN_API getParamStringByValue(Vst::ParamID id, Vst::ParamValue value, Vst::String128 string) override {
        const int control = controlOf(id);
        if (control < 0) return kInvalidArgument;
        const int plain = int(std::lround(normalizedParamToPlain(id, value)));
        setString(string, control == kProgramControl ? programName(std::clamp(plain, 0, 127)) : std::to_string(plain));
        return kResultOk;
    }
    tresult PLUGIN_API getParamValueByString(Vst::ParamID id, Vst::TChar* string, Vst::ParamValue& value) override {
        if (controlOf(id) < 0 || string == nullptr) return kInvalidArgument;
        int plain = 0;
        bool digits = false;
        for (const Vst::TChar* c = string; *c != 0; c++) {
            if (*c < '0' || *c > '9') continue;
            plain = plain * 10 + int(*c - '0');
            digits = true;
            if (plain > 16383) break;
        }
        if (!digits) return kResultFalse;
        value = plainParamToNormalized(id, double(plain));
        return kResultOk;
    }
    Vst::ParamValue PLUGIN_API normalizedParamToPlain(Vst::ParamID id, Vst::ParamValue value) override {
        return std::clamp(value, 0.0, 1.0) * steps(controlOf(id));
    }
    Vst::ParamValue PLUGIN_API plainParamToNormalized(Vst::ParamID id, Vst::ParamValue plain) override {
        const double range = steps(controlOf(id));
        return std::clamp(plain / range, 0.0, 1.0);
    }
    Vst::ParamValue PLUGIN_API getParamNormalized(Vst::ParamID id) override {
        return controlOf(id) >= 0 ? values_[size_t(id - kMidiParamBase)] : 0.0;
    }
    tresult PLUGIN_API setParamNormalized(Vst::ParamID id, Vst::ParamValue value) override {
        if (controlOf(id) < 0) return kInvalidArgument;
        values_[size_t(id - kMidiParamBase)] = std::clamp(value, 0.0, 1.0);
        return kResultOk;
    }
    tresult PLUGIN_API setComponentHandler(Vst::IComponentHandler* handler) override {
        if (handler != nullptr) handler->addRef();
        if (handler_ != nullptr) handler_->release();
        handler_ = handler;
        return kResultOk;
    }
    IPlugView* PLUGIN_API createView(FIDString name) override {
        if (name == nullptr || std::strcmp(name, Vst::ViewType::kEditor) != 0 || !instance_ || !instance_->editor) return nullptr;
        return new View(instance_);
    }

    // IMidiMapping: every controller, channel pressure, pitch bend and the program change of every channel.
    tresult PLUGIN_API getMidiControllerAssignment(int32 busIndex, int16 channel, Vst::CtrlNumber midiControllerNumber,
                                                   Vst::ParamID& id) override {
        if (busIndex != 0 || channel < 0 || channel >= kChannels || midiControllerNumber < 0 || midiControllerNumber >= kMidiControls) {
            return kResultFalse;
        }
        id = kMidiParamBase + Vst::ParamID(channel * kMidiControls + midiControllerNumber);
        return kResultTrue;
    }

    // IUnitInfo: a unit per MIDI channel, each with the timbre memory as its programs.
    int32 PLUGIN_API getUnitCount() override { return 1 + kChannels; }
    tresult PLUGIN_API getUnitInfo(int32 unitIndex, Vst::UnitInfo& info) override {
        if (unitIndex < 0 || unitIndex > kChannels) return kInvalidArgument;
        info.id = unitIndex;
        info.parentUnitId = unitIndex == 0 ? Vst::kNoParentUnitId : Vst::kRootUnitId;
        setString(info.name, unitIndex == 0 ? std::string(kName) : "MIDI channel " + std::to_string(unitIndex));
        info.programListId = unitIndex == 0 ? Vst::kNoProgramListId : kProgramList;
        return kResultOk;
    }
    int32 PLUGIN_API getProgramListCount() override { return 1; }
    tresult PLUGIN_API getProgramListInfo(int32 listIndex, Vst::ProgramListInfo& info) override {
        if (listIndex != 0) return kInvalidArgument;
        info.id = kProgramList;
        setString(info.name, "Timbres");
        info.programCount = 128;
        return kResultOk;
    }
    tresult PLUGIN_API getProgramName(Vst::ProgramListID listId, int32 programIndex, Vst::String128 name) override {
        if (listId != kProgramList || programIndex < 0 || programIndex > 127) return kInvalidArgument;
        setString(name, programName(programIndex));
        return kResultOk;
    }
    tresult PLUGIN_API getProgramInfo(Vst::ProgramListID, int32, Vst::CString, Vst::String128) override { return kResultFalse; }
    tresult PLUGIN_API hasProgramPitchNames(Vst::ProgramListID, int32) override { return kResultFalse; }
    tresult PLUGIN_API getProgramPitchName(Vst::ProgramListID, int32, int16, Vst::String128) override { return kResultFalse; }
    Vst::UnitID PLUGIN_API getSelectedUnit() override { return selectedUnit_; }
    tresult PLUGIN_API selectUnit(Vst::UnitID unitId) override {
        selectedUnit_ = unitId;
        return kResultOk;
    }
    tresult PLUGIN_API getUnitByBus(Vst::MediaType type, Vst::BusDirection dir, int32 busIndex, int32 channel, Vst::UnitID& unitId) override {
        if (type != Vst::kEvent || dir != Vst::kInput || busIndex != 0 || channel < 0 || channel >= kChannels) return kResultFalse;
        unitId = channel + 1;
        return kResultTrue;
    }
    tresult PLUGIN_API setUnitProgramData(int32, int32, IBStream*) override { return kResultFalse; }

    // IConnectionPoint: the processor names its Instance.
    tresult PLUGIN_API connect(Vst::IConnectionPoint* other) override {
        if (other == nullptr) return kInvalidArgument;
        if (peer_ != nullptr) return kResultFalse;
        peer_ = other;
        peer_->addRef();
        return kResultOk;
    }
    tresult PLUGIN_API disconnect(Vst::IConnectionPoint* other) override {
        if (other == nullptr || other != peer_) return kResultFalse;
        peer_->release();
        peer_ = nullptr;
        return kResultOk;
    }
    tresult PLUGIN_API notify(Vst::IMessage* message) override {
        if (message == nullptr || message->getMessageID() == nullptr || std::strcmp(message->getMessageID(), kInstanceMessage) != 0) {
            return kResultFalse;
        }
        int64 id = 0;
        if (message->getAttributes() == nullptr || message->getAttributes()->getInt(kInstanceAttribute, id) != kResultOk) return kResultFalse;
        instance_ = findInstance(id);
        return instance_ ? kResultOk : kResultFalse;
    }

private:
    static int controlOf(Vst::ParamID id) {
        if (id < kMidiParamBase || id >= kMidiParamBase + Vst::ParamID(kParameterCount)) return -1;
        return int(id - kMidiParamBase) % kMidiControls;
    }
    static double steps(int control) { return control == Vst::kPitchBend ? 16383.0 : 127.0; }
    static double defaultValue(int control) {
        switch (control) {
        case Vst::kCtrlVolume: return 100.0 / 127.0;
        case Vst::kCtrlPan: return 64.0 / 127.0;
        case Vst::kCtrlExpression: return 1.0;
        case Vst::kPitchBend: return 8192.0 / 16383.0;
        default: return 0.0;
        }
    }

    std::shared_ptr<Instance> instance_;
    Vst::IConnectionPoint* peer_ = nullptr;
    Vst::IComponentHandler* handler_ = nullptr;
    std::vector<double> values_;
    Vst::UnitID selectedUnit_ = Vst::kRootUnitId;
};

// ---------------------------------------------------------------------------------------------
// The factory

class Factory : public U::ImplementsNonDestroyable<U::Directly<IPluginFactory3>, U::Indirectly<IPluginFactory2, IPluginFactory>> {
public:
    tresult PLUGIN_API getFactoryInfo(PFactoryInfo* info) override {
        if (info == nullptr) return kInvalidArgument;
        *info = PFactoryInfo(kVendor, "", "", PFactoryInfo::kUnicode);
        return kResultOk;
    }
    int32 PLUGIN_API countClasses() override { return 2; }
    tresult PLUGIN_API getClassInfo(int32 index, PClassInfo* info) override {
        if (info == nullptr || index < 0 || index > 1) return kInvalidArgument;
        *info = index == 0 ? PClassInfo(kProcessorCid, PClassInfo::kManyInstances, kVstAudioEffectClass, kName)
                           : PClassInfo(kControllerCid, PClassInfo::kManyInstances, kVstComponentControllerClass, kName);
        return kResultOk;
    }
    tresult PLUGIN_API getClassInfo2(int32 index, PClassInfo2* info) override {
        if (info == nullptr || index < 0 || index > 1) return kInvalidArgument;
        // Not distributable: the processor and the controller share their Instance in one process.
        *info = index == 0 ? PClassInfo2(kProcessorCid, PClassInfo::kManyInstances, kVstAudioEffectClass, kName, 0,
                                         Vst::PlugType::kInstrumentSynth, kVendor, kVersion, kVstVersionString)
                           : PClassInfo2(kControllerCid, PClassInfo::kManyInstances, kVstComponentControllerClass, kName, 0, "",
                                         kVendor, kVersion, kVstVersionString);
        return kResultOk;
    }
    tresult PLUGIN_API getClassInfoUnicode(int32 index, PClassInfoW* info) override {
        if (info == nullptr || index < 0 || index > 1) return kInvalidArgument;
        char16 name[PClassInfo::kNameSize];
        char16 vendor[PClassInfoW::kVendorSize];
        char16 version[PClassInfoW::kVersionSize];
        char16 sdkVersion[PClassInfoW::kVersionSize];
        setString16(name, PClassInfo::kNameSize, kName);
        setString16(vendor, PClassInfoW::kVendorSize, kVendor);
        setString16(version, PClassInfoW::kVersionSize, kVersion);
        setString16(sdkVersion, PClassInfoW::kVersionSize, kVstVersionString);
        *info = index == 0 ? PClassInfoW(kProcessorCid, PClassInfo::kManyInstances, kVstAudioEffectClass, name, 0,
                                         Vst::PlugType::kInstrumentSynth, vendor, version, sdkVersion)
                           : PClassInfoW(kControllerCid, PClassInfo::kManyInstances, kVstComponentControllerClass, name, 0, "", vendor,
                                         version, sdkVersion);
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(FIDString cid, FIDString iid, void** obj) override {
        if (cid == nullptr || iid == nullptr || obj == nullptr) return kInvalidArgument;
        *obj = nullptr;
        FUnknown* created = nullptr;
        if (std::memcmp(cid, kProcessorCid, sizeof(TUID)) == 0) {
            created = static_cast<Vst::IComponent*>(new Processor);
        } else if (std::memcmp(cid, kControllerCid, sizeof(TUID)) == 0) {
            created = static_cast<Vst::IEditController*>(new Controller);
        } else {
            return kNoInterface;
        }
        TUID wanted;
        std::memcpy(wanted, iid, sizeof(TUID));
        const tresult result = created->queryInterface(wanted, obj);
        created->release();  // The caller keeps the reference queryInterface added
        return result;
    }
    tresult PLUGIN_API setHostContext(FUnknown* context) override {
        if (context != nullptr) context->addRef();
        releaseHostContext();
        g_hostContext = context;
        return kResultOk;
    }
};

}  // namespace

// ---------------------------------------------------------------------------------------------
// The module's entry points

#if defined(_WIN32)
#define D110EMU_EXPORT __declspec(dllexport)
#else
#define D110EMU_EXPORT __attribute__((visibility("default")))
#endif

extern "C" {

D110EMU_EXPORT IPluginFactory* PLUGIN_API GetPluginFactory() {
    static Factory factory;
    return &factory;
}

#if defined(_WIN32)
D110EMU_EXPORT bool InitDll() {
    return true;
}

D110EMU_EXPORT bool ExitDll() {
    releaseHostContext();
    return true;
}
#elif defined(__APPLE__)
// A host loads the bundle as a CFBundle and gives it to bundleEntry.
typedef struct __CFBundle* CFBundleRef;

D110EMU_EXPORT bool bundleEntry(CFBundleRef) {
    return true;
}

D110EMU_EXPORT bool bundleExit() {
    releaseHostContext();
    return true;
}
#else
D110EMU_EXPORT bool ModuleEntry(void*) {
    return true;
}

D110EMU_EXPORT bool ModuleExit() {
    releaseHostContext();
    return true;
}
#endif

}  // extern "C"
