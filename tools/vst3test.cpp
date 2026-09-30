// vst3test: D110Emu's VST3 plugin driven the way a host drives it, without a DAW. GetPluginFactory is linked in, and
// the test makes the processor and the controller, connects them (through a proxy, as some hosts do), sets them up and
// calls process() with events, parameter changes and outputs as a host does. Needs the D-110 ROMs, found as the plugin
// finds them (a "roms" folder next to this program or above it). Prints PASS/FAIL; exit code 0 when all pass.
//
// vst3test --module D110Emu.so (Linux) tests a built plugin instead: its library loaded as a host loads it
// (ModuleEntry, GetPluginFactory, ModuleExit), finding the ROMs from where it is. On macOS, vst3test --module
// D110Emu.vst3 loads the bundle as a CFBundle (bundleEntry, GetPluginFactory, bundleExit).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#if defined(__APPLE__)
#define __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES 0  // Keeps check() ours
#include <CoreFoundation/CoreFoundation.h>
#elif !defined(_WIN32)
#include <dlfcn.h>
#endif
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "PluginCore.h"
#include "RolandSysex.h"
#include "TestDataFolder.h"
#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
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

using namespace Steinberg;

extern "C" IPluginFactory* PLUGIN_API GetPluginFactory();

namespace {

// The plugin's factory: the one linked in, or a built plugin's (--module).
IPluginFactory*(PLUGIN_API* g_getFactory)() = &GetPluginFactory;

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) g_failures++;
}

std::string ascii(const Vst::TChar* text) {
    std::string out;
    for (; *text != 0; text++) out += char(*text);
    return out;
}

// ---------------------------------------------------------------------------------------------
// The host's side of the interfaces

class Attributes : public U::Implements<U::Directly<Vst::IAttributeList>> {
public:
    tresult PLUGIN_API setInt(AttrID id, int64 value) override {
        ints_[id] = value;
        return kResultOk;
    }
    tresult PLUGIN_API getInt(AttrID id, int64& value) override {
        const auto found = ints_.find(id);
        if (found == ints_.end()) return kResultFalse;
        value = found->second;
        return kResultOk;
    }
    tresult PLUGIN_API setFloat(AttrID, double) override { return kResultFalse; }
    tresult PLUGIN_API getFloat(AttrID, double&) override { return kResultFalse; }
    tresult PLUGIN_API setString(AttrID, const Vst::TChar*) override { return kResultFalse; }
    tresult PLUGIN_API getString(AttrID, Vst::TChar*, uint32) override { return kResultFalse; }
    tresult PLUGIN_API setBinary(AttrID, const void*, uint32) override { return kResultFalse; }
    tresult PLUGIN_API getBinary(AttrID, const void*&, uint32&) override { return kResultFalse; }

private:
    std::map<std::string, int64> ints_;
};

class Message : public U::Implements<U::Directly<Vst::IMessage>> {
public:
    Message() : attributes_(new Attributes) {}
    ~Message() override { attributes_->release(); }
    FIDString PLUGIN_API getMessageID() override { return id_.c_str(); }
    void PLUGIN_API setMessageID(FIDString id) override { id_ = id; }
    Vst::IAttributeList* PLUGIN_API getAttributes() override { return attributes_; }

private:
    std::string id_;
    Attributes* attributes_;
};

class HostApplication : public U::Implements<U::Directly<Vst::IHostApplication>> {
public:
    tresult PLUGIN_API getName(Vst::String128 name) override {
        const char* text = "vst3test";
        size_t i = 0;
        for (; text[i] != 0; i++) name[i] = char16(text[i]);
        name[i] = 0;
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(TUID cid, TUID iid, void** obj) override {
        if (std::memcmp(cid, Vst::IMessage::iid.toTUID(), sizeof(TUID)) != 0) return kResultFalse;
        messages++;
        Message* message = new Message;
        const tresult result = message->queryInterface(iid, obj);
        message->release();
        return result;
    }
    int messages = 0;
};

// Stands between the processor and the controller, as some hosts put their own connection in between.
class ConnectionProxy : public U::Implements<U::Directly<Vst::IConnectionPoint>> {
public:
    explicit ConnectionProxy(Vst::IConnectionPoint* target) : target_(target) {}
    tresult PLUGIN_API connect(Vst::IConnectionPoint*) override { return kResultOk; }
    tresult PLUGIN_API disconnect(Vst::IConnectionPoint*) override { return kResultOk; }
    tresult PLUGIN_API notify(Vst::IMessage* message) override {
        lastResult = target_->notify(message);
        return lastResult;
    }
    tresult lastResult = kNotInitialized;

private:
    Vst::IConnectionPoint* target_;
};

class MemoryStream : public U::Implements<U::Directly<IBStream>> {
public:
    tresult PLUGIN_API read(void* buffer, int32 numBytes, int32* numBytesRead) override {
        const int32 count = int32(std::min<size_t>(size_t(std::max(numBytes, 0)), data.size() - position));
        std::memcpy(buffer, data.data() + position, size_t(count));
        position += size_t(count);
        if (numBytesRead != nullptr) *numBytesRead = count;
        return kResultOk;
    }
    tresult PLUGIN_API write(void* buffer, int32 numBytes, int32* numBytesWritten) override {
        const auto* bytes = static_cast<const uint8_t*>(buffer);
        data.insert(data.begin() + long(position), bytes, bytes + numBytes);
        position += size_t(numBytes);
        if (numBytesWritten != nullptr) *numBytesWritten = numBytes;
        return kResultOk;
    }
    tresult PLUGIN_API seek(int64 pos, int32 mode, int64* result) override {
        const int64 base = mode == kIBSeekSet ? 0 : mode == kIBSeekCur ? int64(position) : int64(data.size());
        position = size_t(std::clamp<int64>(base + pos, 0, int64(data.size())));
        if (result != nullptr) *result = int64(position);
        return kResultOk;
    }
    tresult PLUGIN_API tell(int64* pos) override {
        if (pos != nullptr) *pos = int64(position);
        return kResultOk;
    }
    std::vector<uint8_t> data;
    size_t position = 0;
};

class EventList : public U::Implements<U::Directly<Vst::IEventList>> {
public:
    int32 PLUGIN_API getEventCount() override { return int32(events.size()); }
    tresult PLUGIN_API getEvent(int32 index, Vst::Event& e) override {
        if (index < 0 || index >= int32(events.size())) return kInvalidArgument;
        e = events[size_t(index)];
        return kResultOk;
    }
    tresult PLUGIN_API addEvent(Vst::Event& e) override {
        events.push_back(e);
        return kResultOk;
    }
    std::vector<Vst::Event> events;
};

class ParamQueue : public U::Implements<U::Directly<Vst::IParamValueQueue>> {
public:
    explicit ParamQueue(Vst::ParamID id) : id_(id) {}
    Vst::ParamID PLUGIN_API getParameterId() override { return id_; }
    int32 PLUGIN_API getPointCount() override { return int32(points.size()); }
    tresult PLUGIN_API getPoint(int32 index, int32& sampleOffset, Vst::ParamValue& value) override {
        if (index < 0 || index >= int32(points.size())) return kInvalidArgument;
        sampleOffset = points[size_t(index)].first;
        value = points[size_t(index)].second;
        return kResultOk;
    }
    tresult PLUGIN_API addPoint(int32 sampleOffset, Vst::ParamValue value, int32& index) override {
        index = int32(points.size());
        points.emplace_back(sampleOffset, value);
        return kResultOk;
    }
    std::vector<std::pair<int32, Vst::ParamValue>> points;

private:
    Vst::ParamID id_;
};

class ParamChanges : public U::Implements<U::Directly<Vst::IParameterChanges>> {
public:
    ~ParamChanges() override { clear(); }
    int32 PLUGIN_API getParameterCount() override { return int32(queues_.size()); }
    Vst::IParamValueQueue* PLUGIN_API getParameterData(int32 index) override {
        return index >= 0 && index < int32(queues_.size()) ? queues_[size_t(index)] : nullptr;
    }
    Vst::IParamValueQueue* PLUGIN_API addParameterData(const Vst::ParamID& id, int32& index) override {
        index = int32(queues_.size());
        queues_.push_back(new ParamQueue(id));
        return queues_.back();
    }
    void add(Vst::ParamID id, int32 offset, double value) {
        int32 index = 0;
        int32 point = 0;
        addParameterData(id, index)->addPoint(offset, value, point);
    }
    void clear() {
        for (ParamQueue* queue : queues_) queue->release();
        queues_.clear();
    }

private:
    std::vector<ParamQueue*> queues_;
};

// ---------------------------------------------------------------------------------------------
// A host with one instance of the plugin

std::vector<uint8_t> dataSet(uint32_t sysexAddress, const std::vector<uint8_t>& data) {
    return RolandSysex::dataSet(0x10, RolandSysex::pack(sysexAddress), data.data(), data.size());  // Unit 17
}

class Host {
public:
    static constexpr int kStereoBuses = 17;  // The mix, then parts 1-8, rhythm and parts 9-15
    static constexpr int kBuses = 23;        // Then MULTI 1-6, mono
    static constexpr int kPairBuses = 20;    // Or MULTI 1+2, 3+4 and 5+6, stereo (d110emu-vst.ini's multi_output_pairs)
    static Vst::SpeakerArrangement arrangement(int bus) { return bus < kStereoBuses ? Vst::SpeakerArr::kStereo : Vst::SpeakerArr::kMono; }

    // activeOutputs: the buses after the mix the host activates (bit n = bus n + 1: part number n's, then MULTI 1-6).
    // pairs: the plugin was made with MULTI 1-6 as stereo pairs (the test has set the computer's choice so).
    explicit Host(double sampleRate = 48000.0, uint32_t activeOutputs = 0, const std::vector<uint8_t>* state = nullptr, bool pairs = false)
        : pairs(pairs), buses(pairs ? kPairBuses : kBuses), context_(new HostApplication) {
        IPluginFactory* factory = g_getFactory();
        PClassInfo processorInfo;
        PClassInfo controllerInfo;
        factory->getClassInfo(0, &processorInfo);
        factory->getClassInfo(1, &controllerInfo);
        factory->createInstance(processorInfo.cid, Vst::IComponent::iid, reinterpret_cast<void**>(&component));
        component->queryInterface(Vst::IAudioProcessor::iid, reinterpret_cast<void**>(&processor));
        component->initialize(context_);
        TUID controllerCid;
        component->getControllerClassId(controllerCid);
        controllerCidMatches = std::memcmp(controllerCid, controllerInfo.cid, sizeof(TUID)) == 0;
        factory->createInstance(controllerCid, Vst::IEditController::iid, reinterpret_cast<void**>(&controller));
        controller->initialize(context_);
        controller->queryInterface(Vst::IMidiMapping::iid, reinterpret_cast<void**>(&mapping));
        controller->queryInterface(Vst::IUnitInfo::iid, reinterpret_cast<void**>(&units));
        component->queryInterface(Vst::IConnectionPoint::iid, reinterpret_cast<void**>(&componentPoint_));
        controller->queryInterface(Vst::IConnectionPoint::iid, reinterpret_cast<void**>(&controllerPoint_));
        toController_ = new ConnectionProxy(controllerPoint_);
        toComponent_ = new ConnectionProxy(componentPoint_);
        componentPoint_->connect(toController_);
        controllerPoint_->connect(toComponent_);
        linked = toController_->lastResult == kResultOk;
        if (state != nullptr) {
            MemoryStream stream;
            stream.data = *state;
            stateLoaded = component->setState(&stream) == kResultOk;
        }
        std::vector<Vst::SpeakerArrangement> arrangements;
        for (int bus = 0; bus < buses; bus++) arrangements.push_back(layout(bus));
        arranged = processor->setBusArrangements(nullptr, 0, arrangements.data(), buses) == kResultTrue;
        Vst::ProcessSetup setup = {Vst::kRealtime, Vst::kSample32, 512, sampleRate};
        processor->setupProcessing(setup);
        for (int bus = 1; bus < buses; bus++) {
            if (((activeOutputs >> (bus - 1)) & 1) != 0) component->activateBus(Vst::kAudio, Vst::kOutput, bus, true);
        }
        component->setActive(true);
        processor->setProcessing(true);
        for (int bus = 0; bus < buses; bus++) {
            left[bus].assign(512, 0.0f);
            right[bus].assign(512, 0.0f);
        }
    }

    ~Host() {
        processor->setProcessing(false);
        component->setActive(false);
        componentPoint_->disconnect(toController_);
        controllerPoint_->disconnect(toComponent_);
        controller->terminate();
        component->terminate();
        toController_->release();
        toComponent_->release();
        componentPoint_->release();
        controllerPoint_->release();
        if (units != nullptr) units->release();
        if (mapping != nullptr) mapping->release();
        controller->release();
        processor->release();
        component->release();
        context_->release();
    }

    // A SysEx message (DataEvent), a note (on with velocity > 0, else off), at a frame of the next block.
    void sysex(int32 frame, const std::vector<uint8_t>& message) {
        sysexData_.push_back(message);
        Vst::Event event = {};
        event.sampleOffset = frame;
        event.type = Vst::Event::kDataEvent;
        event.data.type = Vst::DataEvent::kMidiSysEx;
        event.data.size = uint32(sysexData_.back().size());
        event.data.bytes = sysexData_.back().data();
        events_.events.push_back(event);
    }
    void note(int32 frame, int16 channel, int16 pitch, float velocity) {
        Vst::Event event = {};
        event.sampleOffset = frame;
        if (velocity > 0.0f) {
            event.type = Vst::Event::kNoteOnEvent;
            event.noteOn.channel = channel;
            event.noteOn.pitch = pitch;
            event.noteOn.velocity = velocity;
            event.noteOn.noteId = -1;
        } else {
            event.type = Vst::Event::kNoteOffEvent;
            event.noteOff.channel = channel;
            event.noteOff.pitch = pitch;
            event.noteOff.noteId = -1;
        }
        events_.events.push_back(event);
    }
    // A MIDI controller (or pressure, pitch bend, program change) as the host sends it: a change of the mapped parameter.
    bool controller14(int32 frame, int16 channel, int16 control, double value) {
        Vst::ParamID id = 0;
        if (mapping == nullptr || mapping->getMidiControllerAssignment(0, channel, control, id) != kResultTrue) return false;
        changes_.add(id, frame, value);
        return true;
    }

    // The arrangement of a bus of this plugin's.
    Vst::SpeakerArrangement layout(int bus) const { return pairs ? Vst::SpeakerArr::kStereo : arrangement(bus); }

    // `blocks` blocks of 512 frames; the outputs of each bus collected.
    void run(int blocks) {
        for (int block = 0; block < blocks; block++) {
            float* channels[kBuses][2];
            std::vector<Vst::AudioBusBuffers> outputs(static_cast<size_t>(buses));
            for (int bus = 0; bus < buses; bus++) {
                channels[bus][0] = left[bus].data();
                channels[bus][1] = right[bus].data();
                outputs[size_t(bus)].numChannels = layout(bus) == Vst::SpeakerArr::kStereo ? 2 : 1;
                outputs[size_t(bus)].channelBuffers32 = channels[bus];
            }
            Vst::ProcessData data;
            data.processMode = Vst::kRealtime;
            data.symbolicSampleSize = Vst::kSample32;
            data.numSamples = 512;
            data.numOutputs = buses;
            data.outputs = outputs.data();
            data.inputEvents = &events_;
            data.inputParameterChanges = &changes_;
            processor->process(data);
            for (int bus = 0; bus < buses; bus++) {
                takenLeft[bus].insert(takenLeft[bus].end(), left[bus].begin(), left[bus].end());
                takenRight[bus].insert(takenRight[bus].end(), right[bus].begin(), right[bus].end());
            }
            events_.events.clear();
            changes_.clear();
            sysexData_.clear();
        }
    }

    // RMS of a bus's both channels (-1; a mono MULTI bus's one), or one (0, 1), from frame `from` on.
    double level(int bus, int channel = -1, size_t from = 0) const {
        double sum = 0.0;
        size_t count = 0;
        for (int side = 0; side < 2; side++) {
            if ((channel >= 0 && side != channel) || (side == 1 && layout(bus) != Vst::SpeakerArr::kStereo)) continue;
            const std::vector<float>& samples = side == 0 ? takenLeft[bus] : takenRight[bus];
            for (size_t i = from; i < samples.size(); i++) {
                sum += double(samples[i]) * samples[i];
                count++;
            }
        }
        return count > 0 ? std::sqrt(sum / double(count)) : 0.0;
    }
    // The first frame at or after `from` where the mix is not silent, or -1.
    long onset(size_t from = 0) const {
        for (size_t i = from; i < takenLeft[0].size(); i++) {
            if (std::fabs(takenLeft[0][i]) + std::fabs(takenRight[0][i]) > 1e-5f) return long(i);
        }
        return -1;
    }
    void clearTaken() {
        for (int bus = 0; bus < buses; bus++) {
            takenLeft[bus].clear();
            takenRight[bus].clear();
        }
    }
    std::vector<uint8_t> state() {
        MemoryStream stream;
        component->getState(&stream);
        return stream.data;
    }

    const bool pairs;
    const int buses;
    Vst::IComponent* component = nullptr;
    Vst::IAudioProcessor* processor = nullptr;
    Vst::IEditController* controller = nullptr;
    Vst::IMidiMapping* mapping = nullptr;
    Vst::IUnitInfo* units = nullptr;
    bool controllerCidMatches = false;
    bool linked = false;
    bool arranged = false;
    bool stateLoaded = false;
    std::vector<float> left[kBuses], right[kBuses];
    std::vector<float> takenLeft[kBuses], takenRight[kBuses];

private:
    HostApplication* context_;
    Vst::IConnectionPoint* componentPoint_ = nullptr;
    Vst::IConnectionPoint* controllerPoint_ = nullptr;
    ConnectionProxy* toController_ = nullptr;
    ConnectionProxy* toComponent_ = nullptr;
    EventList events_;
    ParamChanges changes_;
    std::vector<std::vector<uint8_t>> sysexData_;
};

// ---------------------------------------------------------------------------------------------

void testFactory() {
    IPluginFactory* factory = g_getFactory();
    PFactoryInfo info;
    factory->getFactoryInfo(&info);
    IPluginFactory2* factory2 = nullptr;
    IPluginFactory3* factory3 = nullptr;
    factory->queryInterface(IPluginFactory2::iid, reinterpret_cast<void**>(&factory2));
    factory->queryInterface(IPluginFactory3::iid, reinterpret_cast<void**>(&factory3));
    PClassInfo2 processor;
    PClassInfo2 controller;
    bool infos = factory2 != nullptr && factory2->getClassInfo2(0, &processor) == kResultOk && factory2->getClassInfo2(1, &controller) == kResultOk;
    PClassInfoW unicode;
    const bool wide = factory3 != nullptr && factory3->getClassInfoUnicode(0, &unicode) == kResultOk && unicode.name[0] == char16('D');
    std::printf("      factory: vendor \"%s\", %d classes; \"%s\" (%s, %s), \"%s\" (%s)\n", info.vendor, factory->countClasses(), processor.name,
                processor.category, processor.subCategories, controller.name, controller.category);
    infos = infos && std::string(processor.category) == kVstAudioEffectClass && std::string(processor.subCategories) == "Instrument|Synth" &&
            std::string(controller.category) == kVstComponentControllerClass && processor.classFlags == 0;
    check(factory->countClasses() == 2 && std::string(info.vendor) == "D110Emu" && infos && wide,
          "factory: an instrument (Instrument|Synth) and its controller, not distributable; Unicode class infos");
    if (factory2 != nullptr) factory2->release();
    if (factory3 != nullptr) factory3->release();
}

void testSetup() {
    Host host;
    Vst::BusInfo bus;
    std::vector<std::string> names;
    bool buses = host.component->getBusCount(Vst::kAudio, Vst::kOutput) == Host::kBuses &&
                 host.component->getBusCount(Vst::kEvent, Vst::kInput) == 1 && host.component->getBusCount(Vst::kAudio, Vst::kInput) == 0;
    for (int i = 0; i < Host::kBuses; i++) {
        host.component->getBusInfo(Vst::kAudio, Vst::kOutput, i, bus);
        names.push_back(ascii(bus.name));
        Vst::SpeakerArrangement arrangement = 0;
        buses = buses && bus.channelCount == (i < Host::kStereoBuses ? 2 : 1) && (i == 0) == (bus.busType == Vst::kMain) &&
                (bus.flags & Vst::BusInfo::kDefaultActive) != 0 &&
                host.processor->getBusArrangement(Vst::kOutput, i, arrangement) == kResultOk && arrangement == Host::arrangement(i);
    }
    host.component->getBusInfo(Vst::kEvent, Vst::kInput, 0, bus);
    buses = buses && bus.channelCount == 16 && names[0] == "D-110 Mix" && names[1] == "Part 1" && names[8] == "Part 8" && names[9] == "Rhythm" &&
            names[10] == "Part 9" && names[16] == "Part 15" && names[17] == "Multi 1" && names[22] == "Multi 6" &&
            host.component->getBusInfo(Vst::kAudio, Vst::kOutput, Host::kBuses, bus) != kResultOk;
    std::printf("      outputs: %s, %s, ..., %s, %s, %s ... %s, %s ... %s\n", names[0].c_str(), names[1].c_str(), names[8].c_str(),
                names[9].c_str(), names[10].c_str(), names[16].c_str(), names[17].c_str(), names[22].c_str());
    // Only our arrangements: a mono part output, or a stereo MULTI output, is refused.
    std::vector<Vst::SpeakerArrangement> monoPart, stereoMulti;
    for (int i = 0; i < Host::kBuses; i++) {
        monoPart.push_back(Host::arrangement(i));
        stereoMulti.push_back(Host::arrangement(i));
    }
    monoPart[3] = Vst::SpeakerArr::kMono;
    stereoMulti[20] = Vst::SpeakerArr::kStereo;
    const bool ours = host.arranged && host.processor->setBusArrangements(nullptr, 0, monoPart.data(), Host::kBuses) == kResultFalse &&
                      host.processor->setBusArrangements(nullptr, 0, stereoMulti.data(), Host::kBuses) == kResultFalse &&
                      host.processor->setBusArrangements(nullptr, 0, monoPart.data(), 1) == kResultTrue &&  // The mix alone
                      host.processor->canProcessSampleSize(Vst::kSample64) == kResultFalse;
    check(buses && ours && host.controllerCidMatches && host.linked,
          "the processor: a stereo mix, 16 stereo part outputs and 6 mono MULTI outputs (all active by default), a "
          "16-channel MIDI input; the controller found, and linked to the processor through the host's connection and "
          "message");

    // The MIDI mapping: every controller, pressure, pitch bend and program change on every channel; the units.
    Vst::ParamID volume = 0;
    Vst::ParamID bend = 0;
    Vst::ParamID program = 0;
    Vst::ParamID other = 0;
    const bool mapped = host.mapping->getMidiControllerAssignment(0, 0, Vst::kCtrlVolume, volume) == kResultTrue &&
                        host.mapping->getMidiControllerAssignment(0, 15, Vst::kPitchBend, bend) == kResultTrue &&
                        host.mapping->getMidiControllerAssignment(0, 4, Vst::kCtrlProgramChange, program) == kResultTrue &&
                        host.mapping->getMidiControllerAssignment(1, 0, Vst::kCtrlVolume, other) == kResultFalse && volume != bend;
    const int32 parameters = host.controller->getParameterCount();
    Vst::ParameterInfo programInfo = {};
    for (int32 i = 0; i < parameters; i++) {
        Vst::ParameterInfo info;
        if (host.controller->getParameterInfo(i, info) == kResultOk && info.id == program) programInfo = info;
    }
    Vst::UnitID unit = -1;
    Vst::ProgramListInfo list = {};
    Vst::String128 name = {};
    const bool unitsOk = host.units->getUnitCount() == 17 && host.units->getUnitByBus(Vst::kEvent, Vst::kInput, 0, 4, unit) == kResultTrue &&
                         unit == 5 && host.units->getProgramListInfo(0, list) == kResultOk && list.programCount == 128 &&
                         host.units->getProgramName(list.id, 9, name) == kResultOk && ascii(name) == "I-A22";
    Vst::String128 display = {};
    host.controller->getParamStringByValue(volume, host.controller->getParamNormalized(volume), display);
    std::printf("      %d parameters; channel 1's volume shows %s; program change of channel 5: unit %d, flags %x; program 10: %s\n",
                parameters, ascii(display).c_str(), programInfo.unitId, unsigned(programInfo.flags), ascii(name).c_str());
    check(mapped && parameters == 16 * 131 && (programInfo.flags & Vst::ParameterInfo::kIsProgramChange) != 0 && programInfo.unitId == 5 &&
              unitsOk && ascii(display) == "100",
          "the controller maps every MIDI controller, pressure, pitch bend and program change of every channel to a parameter; "
          "a unit per channel with the timbres (I-A11 - I-B88) as programs");
}

// Part 1 on Mix (dry, by a SysEx data event) plays on channel 1, part 2 on channel 2.
void playParts(Host& host, bool part2) {
    host.sysex(0, dataSet(0x030006, {0}));  // Part 1's output assign: Mix
    host.run(1);
    host.note(100, 0, 60, 0.8f);
    if (part2) host.note(100, 1, 64, 0.8f);
    host.run(40);
}

// The project's parts on their own outputs (own_outputs, which the Output menus set): part 1.
std::vector<uint8_t> partOneOwnState() {
    return PluginCore::encodeState("own_outputs = 1\n", {});
}

void testPartOutputs() {
    const std::vector<uint8_t> own = partOneOwnState();
    Host routed(48000.0, 0u, &own);  // Part 1 on its own output, whatever outputs the host has taken
    playParts(routed, false);
    const double part1Own = routed.level(1), part1Mix = routed.level(0);
    Host both(48000.0, 0u, &own);
    playParts(both, true);
    Host plain;  // In the mix
    playParts(plain, false);
    std::printf("      part 1 on its own output: own output %.4f, mix %.6f; with part 2: mix %.4f; in the mix: mix %.4f, own output %.6f\n",
                part1Own, part1Mix, both.level(0), plain.level(0), plain.level(1));
    check(part1Own > 0.001 && part1Mix < 1e-6 && both.level(0) > 0.001 && both.level(1) > 0.001 && plain.level(0) > 0.001 &&
              plain.level(1) == 0.0,
          "part outputs: a part the project puts on its own output plays out of it (Part 1), whatever the host activates; "
          "otherwise in the mix");

    // MULTI outputs: part 1 on MULTI 2 plays out of the Multi 2 bus (mono), not the mix; the other MULTI buses stay silent.
    Host multi;
    multi.sysex(0, dataSet(0x030006, {3}));  // Part 1's output assign: MULTI 2
    multi.run(1);
    multi.note(100, 0, 60, 0.8f);
    multi.run(40);
    std::string levels;
    bool othersSilent = true;
    for (int n = 0; n < 6; n++) {
        char text[16];
        std::snprintf(text, sizeof(text), "%s%.4f", n == 0 ? "" : " ", multi.level(Host::kStereoBuses + n));
        levels += text;
        if (n != 1) othersSilent = othersSilent && multi.level(Host::kStereoBuses + n) == 0.0;
    }
    std::printf("      part 1 on MULTI 2: Multi 1-6 %s, the mix %.6f\n", levels.c_str(), multi.level(0));
    check(multi.level(Host::kStereoBuses + 1) > 0.001 && othersSilent && multi.level(0) < 1e-6,
          "MULTI outputs: a part on MULTI 2 plays out of the Multi 2 bus, mono, not in the mix");
}

// MULTI 1-6 as three stereo pairs, the computer's choice for new instances (d110emu-vst.ini, which the configuration window
// sets): 20 stereo buses, the last Multi 1+2, 3+4 and 5+6; a part on MULTI 3 plays on the left of Multi 3+4, a rhythm key
// on MULTI 6 on the right of Multi 5+6.
void testMultiPairs() {
    setTestPluginSetting("multi_output_pairs", "true");
    {
        Host host(48000.0, 0u, nullptr, true);
        Vst::BusInfo bus;
        std::vector<std::string> names;
        bool buses = host.component->getBusCount(Vst::kAudio, Vst::kOutput) == Host::kPairBuses;
        for (int i = 0; buses && i < Host::kPairBuses; i++) {
            Vst::SpeakerArrangement arrangement = 0;
            buses = host.component->getBusInfo(Vst::kAudio, Vst::kOutput, i, bus) == kResultOk && bus.channelCount == 2 &&
                    host.processor->getBusArrangement(Vst::kOutput, i, arrangement) == kResultOk && arrangement == Vst::SpeakerArr::kStereo;
            names.push_back(ascii(bus.name));
        }
        buses = buses && names.size() == 20 && names[16] == "Part 15" && names[17] == "Multi 1+2" && names[18] == "Multi 3+4" &&
                names[19] == "Multi 5+6" && host.component->getBusInfo(Vst::kAudio, Vst::kOutput, Host::kPairBuses, bus) != kResultOk;
        std::vector<Vst::SpeakerArrangement> monoMulti(Host::kPairBuses, Vst::SpeakerArr::kStereo);
        monoMulti[18] = Vst::SpeakerArr::kMono;
        const bool ours = host.arranged && host.processor->setBusArrangements(nullptr, 0, monoMulti.data(), Host::kPairBuses) == kResultFalse;
        // Stereo pairs keep the pan: part 1 on MULTI 4 and the bass drum on MULTI 6, both panned hard left, play on the
        // left of their pairs (as mono outputs, MULTI 4 and 6 would be the right channels).
        host.sysex(0, dataSet(0x030006, {5}));  // Part 1's output assign: MULTI 4
        host.sysex(0, dataSet(0x030009, {0}));  // Its panpot: hard left
        host.sysex(0, dataSet(0x030142, {0, 7}));  // The bass drum's (key 36): hard left, MULTI 6
        host.run(1);
        host.note(100, 0, 60, 0.8f);
        host.note(100, 9, 36, 0.8f);
        host.run(40);
        std::printf("      buses %s ... %s, %s, %s; part 1 on MULTI 4, panned left: Multi 3+4 left %.4f, right %.6f; the bass drum on "
                    "MULTI 6, panned left: Multi 5+6 left %.4f, right %.6f; Multi 1+2 %.6f, the mix %.6f\n",
                    names.empty() ? "?" : names[0].c_str(), names.size() < 20 ? "?" : names[17].c_str(), names.size() < 20 ? "?" : names[18].c_str(),
                    names.size() < 20 ? "?" : names[19].c_str(), host.level(18, 0), host.level(18, 1), host.level(19, 0), host.level(19, 1),
                    host.level(17), host.level(0));
        check(buses && ours && host.level(18, 0) > 0.001 && host.level(18, 1) == 0.0 && host.level(19, 0) > 0.001 &&
                  host.level(19, 1) == 0.0 && host.level(17) == 0.0 && host.level(0) < 1e-6,
              "MULTI outputs as stereo pairs (the computer's choice): 20 stereo buses, Multi 1+2, 3+4 and 5+6 last; notes on "
              "either output of a pair play there with their pan (part 1 on MULTI 4 and a rhythm key on MULTI 6, panned left, "
              "on the left of Multi 3+4 and Multi 5+6)");
    }
    setTestPluginSetting("multi_output_pairs", "false");
    Host mono;
    check(mono.component->getBusCount(Vst::kAudio, Vst::kOutput) == Host::kBuses, "Back to six mono MULTI outputs for new instances");
}

void testMidi() {
    // A controller as a parameter change: channel 1's volume at 0 quietens part 1 (MIX + reverb, its default).
    Host loud;
    loud.note(0, 0, 60, 0.8f);
    loud.run(20);
    Host quiet;
    const bool mapped = quiet.controller14(0, 0, Vst::kCtrlVolume, 0.0);
    quiet.note(0, 0, 60, 0.8f);
    quiet.run(20);
    // Program change as a parameter change: part 1's timbre temporary area (in the state's memory) follows.
    Host programs;
    const std::vector<uint8_t> before = programs.state();
    programs.controller14(0, 0, Vst::kCtrlProgramChange, 20.0 / 127.0);
    programs.run(2);
    const std::vector<uint8_t> after = programs.state();
    // Sample-accurate notes: at frames 50 and 300 of a block, as in the VST2 plugin.
    Host timing;
    timing.run(2);
    timing.clearTaken();
    timing.note(300, 0, 72, 1.0f);
    timing.run(1);
    const long onset = timing.onset();
    std::printf("      volume 0 by parameter change: %.6f (without %.4f); program change changes the state %d; note at frame 300 "
                "sounds from %ld\n",
                quiet.level(0), loud.level(0), before != after, onset);
    // (Volume 0 is the D-110's quietest, about 45 dB down, not silence.)
    check(mapped && loud.level(0) > 0.001 && quiet.level(0) < loud.level(0) * 0.01 && before != after && onset >= 300 && onset <= 304,
          "MIDI: controllers and program changes arrive as parameter changes and play as MIDI; notes at their frame");
}

void testState() {
    // Part 1 on its own output and on Mix in one instance; another loaded from its state plays part 1 out of its output.
    const std::vector<uint8_t> own = partOneOwnState();
    Host first(48000.0, 0u, &own);
    first.sysex(0, dataSet(0x030006, {0}));
    first.run(2);
    const std::vector<uint8_t> state = first.state();
    Host second(44100.0, 0u, &state);
    second.run(1);
    second.note(100, 0, 60, 0.8f);
    second.run(40);
    std::printf("      state %zu bytes, loaded %d; at 44.1 kHz part 1 plays on its own output %.4f, mix %.6f\n", state.size(),
                second.stateLoaded, second.level(1), second.level(0));
    check(!state.empty() && second.stateLoaded && second.level(1) > 0.001 && second.level(0) < 1e-6,
          "the project state (getState/setState) round trips; a part's own output at 44.1 kHz (resampled in step with the mix)");
}

#if defined(__APPLE__)
// A built plugin's bundle, loaded as hosts load it: the .vst3 folder, or the program in its Contents/MacOS.
CFBundleRef loadBundle(std::string path) {
    while (!path.empty() && path.back() == '/') path.pop_back();
    const size_t contents = path.rfind("/Contents/MacOS/");
    if (contents != std::string::npos) path.erase(contents);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(path.c_str()),
                                                           CFIndex(path.size()), true);
    CFBundleRef bundle = url != nullptr ? CFBundleCreate(kCFAllocatorDefault, url) : nullptr;
    if (url != nullptr) CFRelease(url);
    if (bundle != nullptr && !CFBundleLoadExecutable(bundle)) {
        CFRelease(bundle);
        bundle = nullptr;
    }
    return bundle;
}
#endif

}  // namespace

int main(int argc, char** argv) {
    useTestDataFolder("d110emu-vst3test", true);  // Not the user's own plugin settings, for the built plugin too
#if defined(__APPLE__)
    CFBundleRef bundle = nullptr;
    bool (*bundleExit)() = nullptr;
    if (argc == 3 && std::strcmp(argv[1], "--module") == 0) {
        bundle = loadBundle(argv[2]);
        if (bundle == nullptr) {
            std::fprintf(stderr, "Cannot load the bundle %s\n", argv[2]);
            return 2;
        }
        const auto bundleEntry = reinterpret_cast<bool (*)(CFBundleRef)>(CFBundleGetFunctionPointerForName(bundle, CFSTR("bundleEntry")));
        bundleExit = reinterpret_cast<bool (*)()>(CFBundleGetFunctionPointerForName(bundle, CFSTR("bundleExit")));
        g_getFactory = reinterpret_cast<IPluginFactory*(PLUGIN_API*)()>(CFBundleGetFunctionPointerForName(bundle, CFSTR("GetPluginFactory")));
        if (bundleEntry == nullptr || bundleExit == nullptr || g_getFactory == nullptr || !bundleEntry(bundle)) {
            std::fprintf(stderr, "%s lacks the VST3 entry points, or bundleEntry failed\n", argv[2]);
            return 2;
        }
        std::printf("      bundle %s\n", argv[2]);
    } else if (argc != 1) {
        std::fprintf(stderr, "Usage: vst3test [--module D110Emu.vst3]\n");
        return 2;
    }
#elif !defined(_WIN32)
    void* module = nullptr;
    bool (*moduleExit)() = nullptr;
    if (argc == 3 && std::strcmp(argv[1], "--module") == 0) {
        module = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
        if (module == nullptr) {
            std::fprintf(stderr, "Cannot load %s: %s\n", argv[2], dlerror());
            return 2;
        }
        const auto moduleEntry = reinterpret_cast<bool (*)(void*)>(dlsym(module, "ModuleEntry"));
        moduleExit = reinterpret_cast<bool (*)()>(dlsym(module, "ModuleExit"));
        g_getFactory = reinterpret_cast<IPluginFactory*(PLUGIN_API*)()>(dlsym(module, "GetPluginFactory"));
        if (moduleEntry == nullptr || moduleExit == nullptr || g_getFactory == nullptr || !moduleEntry(module)) {
            std::fprintf(stderr, "%s lacks the VST3 entry points, or ModuleEntry failed\n", argv[2]);
            return 2;
        }
        std::printf("      module %s\n", argv[2]);
    } else if (argc != 1) {
        std::fprintf(stderr, "Usage: vst3test [--module D110Emu.so]\n");
        return 2;
    }
#else
    (void)argc;
    (void)argv;
#endif
    testFactory();
    testSetup();
    testPartOutputs();
    testMultiPairs();
    testMidi();
    testState();
#if defined(__APPLE__)
    if (bundle != nullptr) {
        bundleExit();
        CFRelease(bundle);  // Loaded for good, as hosts leave them: code with Objective-C classes is not unloaded
    }
#elif !defined(_WIN32)
    if (module != nullptr) {
        moduleExit();
        dlclose(module);
    }
#endif
    std::printf("%s: %d failures\n", g_failures == 0 ? "ALL PASS" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
