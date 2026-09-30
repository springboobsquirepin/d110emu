// D110Emu as an Audio Unit (AUv2) instrument, for Logic Pro, GarageBand, MainStage and the other Audio Unit hosts on
// macOS: PluginCore (the App, as the VST plugins run it) behind Apple's AudioUnitSDK (vendor/AudioUnitSDK: a
// MusicDeviceBase does the Audio Unit's side), with the Mac editor (PluginEditorMac.mm) as its Cocoa view.
//
// The outputs: the mix, a stereo output per part, then MULTI 1-6 as three stereo pairs (Logic Pro cannot load a plugin
// with mono outputs beside stereo ones). MIDI comes as bytes, SysEx included where a host sends it (Logic Pro sends none
// to AUv2 instruments). The state is the VST plugins' (PluginCore::saveState), inside the Audio Unit's ClassInfo.
//
// Compiled as C++23, which the SDK needs, with ARC.

// Carbon's AssertMacros.h (through AudioToolbox) would otherwise define check(), verify() and require() as macros.
#define __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES 0

#include <AudioUnitSDK/AUConfig.h>  // The SDK's first
#include <AudioUnitSDK/AUPlugInDispatch.h>
#include <AudioUnitSDK/MusicDeviceBase.h>

#import <AudioUnit/AUCocoaUIView.h>
#import <Cocoa/Cocoa.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Platform.h"
#include "PluginCore.h"

namespace {

// The unit's instance for its view, through a property of ours (IDs from 64000 are a unit's own): an AUv2 view runs in
// its unit's process.
constexpr AudioUnitPropertyID kInstanceProperty = 64000;
// The mix, parts 1-8, rhythm and parts 9-15, then MULTI 1+2, 3+4 and 5+6: all stereo.
constexpr UInt32 kOutputBuses = 1 + SynthEngine::kPartOutputs + SynthEngine::kMultiPairs;
constexpr Float64 kTailSeconds = 4.0;  // The reverb and the releases after the last note
const char* const kStateKey = "D110EmuState";  // Our state in the ClassInfo dictionary
// The view's factory class, which the host looks up by name in this bundle.
const char* const kViewFactoryClass = "D110EmuAUViewFactory";

// The core and its editor, which the unit and its view share: the view may outlive the unit a moment.
struct AuInstance {
    AuInstance() : core(PluginFormat::Au), editor(createPluginEditor(core)) {
        if (editor) core.setEditor(editor.get());
    }
    ~AuInstance() {
        core.setEditor(nullptr);
        editor.reset();  // Closes the window before the core goes
    }
    AuInstance(const AuInstance&) = delete;
    AuInstance& operator=(const AuInstance&) = delete;

    PluginCore core;
    std::unique_ptr<PluginEditor> editor;
    void* view = nullptr;  // The view the editor is open in (a host may ask for another before it lets go of one)
};

std::string busName(UInt32 bus) {
    if (bus == 0) return "D-110 Mix";
    if (bus <= UInt32(SynthEngine::kPartOutputs)) {
        const int part = int(bus) - 1;  // Part numbers: parts 1-8, rhythm (8), parts 9-15
        return part == 8 ? std::string("Rhythm") : "Part " + std::to_string(part < 8 ? part + 1 : part);
    }
    const int first = 2 * int(bus - 1 - UInt32(SynthEngine::kPartOutputs)) + 1;
    return "Multi " + std::to_string(first) + "+" + std::to_string(first + 1);
}

CFStringRef copyString(const std::string& text) {
    return CFStringCreateWithCString(kCFAllocatorDefault, text.c_str(), kCFStringEncodingUTF8);
}

// This bundle (D110Emu.component, around Contents/MacOS/D110Emu), where the host finds the view's factory class.
CFURLRef copyBundleUrl() {
    const std::string path = Platform::moduleDirectory().parent_path().parent_path().string();
    return CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(path.data()), CFIndex(path.size()),
                                                   true);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// The unit (at global scope: the SDK's factory macro names its entry point after the class)

class D110EmuAU : public ausdk::MusicDeviceBase {
public:
    explicit D110EmuAU(AudioComponentInstance component)
        : MusicDeviceBase(component, 0, kOutputBuses), instance_(std::make_shared<AuInstance>()) {}

    // After construction, with the outputs made: their names.
    void PostConstructor() override {
        MusicDeviceBase::PostConstructor();
        for (UInt32 bus = 0; bus < kOutputBuses; bus++) {
            CFStringRef name = copyString(busName(bus));
            if (name == nullptr) continue;
            Output(bus).SetName(name);  // Retained there
            CFRelease(name);
        }
    }

    OSStatus Initialize() override {
        const OSStatus result = MusicDeviceBase::Initialize();
        if (result != noErr) return result;
        // The App (and its ROMs) now, at the host's rate: not while a host merely looks the unit over.
        instance_->core.setSampleRate(Output(0).GetStreamFormat().mSampleRate);
        instance_->core.activate();
        return noErr;
    }

    // Formats: 32-bit float, two channels on every output, one rate for all (set while uninitialized).
    bool StreamFormatWritable(AudioUnitScope /*scope*/, AudioUnitElement /*element*/) override { return !IsInitialized(); }
    bool ValidFormat(AudioUnitScope scope, AudioUnitElement element, const AudioStreamBasicDescription& format) override {
        return MusicDeviceBase::ValidFormat(scope, element, format) && format.mChannelsPerFrame == 2;
    }
    OSStatus ChangeStreamFormat(AudioUnitScope scope, AudioUnitElement element, const AudioStreamBasicDescription& previous,
                                const AudioStreamBasicDescription& format) override {
        const OSStatus result = MusicDeviceBase::ChangeStreamFormat(scope, element, previous, format);
        if (result != noErr || format.mSampleRate == previous.mSampleRate) return result;
        // A host may set the rate on one output only: the others follow.
        for (UInt32 bus = 0; bus < kOutputBuses; bus++) {
            AudioStreamBasicDescription other = Output(bus).GetStreamFormat();
            if (other.mSampleRate == format.mSampleRate) continue;
            other.mSampleRate = format.mSampleRate;
            if (Output(bus).SetStreamFormat(other) == noErr) PropertyChanged(kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, bus);
        }
        return noErr;
    }
    UInt32 SupportedNumChannels(const AUChannelInfo** info) override {
        static const AUChannelInfo kChannels[] = {{0, 2}};  // No inputs, stereo outputs
        if (info != nullptr) *info = kChannels;
        return 1;
    }

    bool CanScheduleParameters() const AUSDK_RTSAFE override { return false; }  // It has none
    bool SupportsTail() AUSDK_RTSAFE override { return true; }
    Float64 GetTailTime() AUSDK_RTSAFE override { return kTailSeconds; }

    OSStatus GetPropertyInfo(AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement element, UInt32& size, bool& writable) override {
        if (scope == kAudioUnitScope_Global && (id == kAudioUnitProperty_CocoaUI || id == kInstanceProperty)) {
            size = id == kAudioUnitProperty_CocoaUI ? UInt32(sizeof(AudioUnitCocoaViewInfo)) : UInt32(sizeof(void*));
            writable = false;
            return noErr;
        }
        return MusicDeviceBase::GetPropertyInfo(id, scope, element, size, writable);
    }
    OSStatus GetProperty(AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement element, void* data) override {
        if (scope == kAudioUnitScope_Global && id == kAudioUnitProperty_CocoaUI) {
            // The view's factory class and its bundle, which the host releases.
            auto* info = static_cast<AudioUnitCocoaViewInfo*>(data);
            info->mCocoaAUViewBundleLocation = copyBundleUrl();
            info->mCocoaAUViewClass[0] = copyString(kViewFactoryClass);
            const bool made = info->mCocoaAUViewBundleLocation != nullptr && info->mCocoaAUViewClass[0] != nullptr;
            return made ? OSStatus(noErr) : OSStatus(kAudioUnitErr_InvalidProperty);
        }
        if (scope == kAudioUnitScope_Global && id == kInstanceProperty) {
            *static_cast<std::shared_ptr<AuInstance>**>(data) = &instance_;
            return noErr;
        }
        return MusicDeviceBase::GetProperty(id, scope, element, data);
    }

    // Every output at once, each into its own buffer: the host takes the others from there in this cycle (the SDK's
    // RenderBus renders once per time stamp).
    OSStatus Render(AudioUnitRenderActionFlags& /*flags*/, const AudioTimeStamp& /*time*/, UInt32 frames) AUSDK_RTSAFE override {
        float* channels[kOutputBuses][2] = {};
        for (UInt32 bus = 0; bus < kOutputBuses; bus++) {
            const ausdk::ExpectedPtr<ausdk::AUOutputElement> output = GetOutputOrError(bus);
            if (!output || !output->PrepareBufferOrError(frames)) continue;
            channels[bus][0] = output->GetFloat32ChannelDataRT(0);
            channels[bus][1] = output->GetFloat32ChannelDataRT(1);
        }
        float* partLeft[SynthEngine::kPartOutputs] = {};
        float* partRight[SynthEngine::kPartOutputs] = {};
        for (int part = 0; part < SynthEngine::kPartOutputs; part++) {
            partLeft[part] = channels[1 + part][0];
            partRight[part] = channels[1 + part][1];
        }
        // MULTI 1 and 2 on the left and right of the first pair's output, and so on.
        float* multi[PluginCore::kMultiOutputs] = {};
        for (int output = 0; output < PluginCore::kMultiOutputs; output++) {
            multi[output] = channels[1 + SynthEngine::kPartOutputs + output / 2][output % 2];
        }
        AUSDK_RT_UNSAFE_BEGIN("The core takes two locks that are only held briefly")
        instance_->core.render(channels[0][0], channels[0][1], partLeft, partRight, multi, frames, false);
        AUSDK_RT_UNSAFE_END
        return noErr;
    }

    // The host's MIDI, as bytes; SysEx comes without a time, and plays at the start of the next block.
    OSStatus HandleMIDIEvent(UInt8 status, UInt8 channel, UInt8 data1, UInt8 data2, UInt32 startFrame) AUSDK_RTSAFE override {
        if (!IsInitialized()) return kAudioUnitErr_Uninitialized;
        AUSDK_RT_UNSAFE_BEGIN("A lock held only to queue the message")
        instance_->core.queueMidi(startFrame, UInt8(status | channel), data1, data2);
        AUSDK_RT_UNSAFE_END
        return noErr;
    }
    OSStatus HandleSysEx(const UInt8* data, UInt32 length) AUSDK_RTSAFE override {
        AUSDK_RT_UNSAFE_BEGIN("A lock held only to queue the message")
        instance_->core.queueSysex(0, data, length);
        AUSDK_RT_UNSAFE_END
        return noErr;
    }

    // The project's state: the SDK's ClassInfo dictionary, with ours in it.
    OSStatus SaveState(CFPropertyListRef* outData) override {
        const OSStatus result = MusicDeviceBase::SaveState(outData);
        if (result != noErr || outData == nullptr || *outData == nullptr) return result;
        const std::vector<uint8_t> state = instance_->core.saveState();
        CFDataRef data = CFDataCreate(kCFAllocatorDefault, state.data(), CFIndex(state.size()));
        CFStringRef key = copyString(kStateKey);
        if (data != nullptr && key != nullptr) {
            CFDictionarySetValue(static_cast<CFMutableDictionaryRef>(const_cast<void*>(*outData)), key, data);
        }
        if (data != nullptr) CFRelease(data);
        if (key != nullptr) CFRelease(key);
        return noErr;
    }
    OSStatus RestoreState(CFPropertyListRef plist) override {
        const OSStatus result = MusicDeviceBase::RestoreState(plist);
        if (result != noErr) return result;
        if (plist == nullptr || CFGetTypeID(plist) != CFDictionaryGetTypeID()) return noErr;
        CFStringRef key = copyString(kStateKey);
        const void* value = key != nullptr ? CFDictionaryGetValue(static_cast<CFDictionaryRef>(plist), key) : nullptr;
        if (key != nullptr) CFRelease(key);
        if (value == nullptr || CFGetTypeID(value) != CFDataGetTypeID()) return noErr;  // A preset of another kind: the unit's as it is
        const auto data = static_cast<CFDataRef>(value);
        const bool loaded = instance_->core.loadState(CFDataGetBytePtr(data), size_t(CFDataGetLength(data)));
        return loaded ? OSStatus(noErr) : OSStatus(kAudioUnitErr_InvalidPropertyValue);
    }

private:
    std::shared_ptr<AuInstance> instance_;
};

AUSDK_COMPONENT_ENTRY(ausdk::AUMusicDeviceFactory, D110EmuAU)

// ---------------------------------------------------------------------------------------------
// The view: the host puts it in its window; the editor opens inside it and closes when the host lets go of it.

@interface D110EmuAUView : NSView
- (instancetype)initWithInstance:(const std::shared_ptr<AuInstance>&)instance;
@end

@implementation D110EmuAUView {
    std::shared_ptr<AuInstance> _instance;
}

- (instancetype)initWithInstance:(const std::shared_ptr<AuInstance>&)instance {
    int width = 0;
    int height = 0;
    instance->editor->size(width, height);
    if ((self = [super initWithFrame:NSMakeRect(0, 0, width, height)])) {
        _instance = instance;
        PluginEditor& editor = *_instance->editor;
        // View > Window size: this view takes the new size, and hosts follow their view's frame.
        __unsafe_unretained D110EmuAUView* view = self;
        editor.setHostResize([view](int newWidth, int newHeight) {
            [view setFrameSize:NSMakeSize(newWidth, newHeight)];
            return true;
        });
        _instance->view = (__bridge void*)self;
        if (!editor.open((__bridge void*)self)) {
            editor.setHostResize(nullptr);
            _instance->view = nullptr;
            return nil;
        }
    }
    return self;
}

- (void)dealloc {
    // The host let go of the view: the editor closes, unless another view of the host's has it by now.
    if (_instance && _instance->editor && _instance->view == (__bridge void*)self) {
        _instance->editor->close();
        _instance->editor->setHostResize(nullptr);
        _instance->view = nullptr;
    }
}

@end

// The host makes one of these to ask for views (kAudioUnitProperty_CocoaUI names it).
@interface D110EmuAUViewFactory : NSObject <AUCocoaUIBase>
@end

@implementation D110EmuAUViewFactory

- (unsigned)interfaceVersion {
    return 0;
}

- (NSString*)description {
    return @"D110Emu";
}

- (NSView*)uiViewForAudioUnit:(AudioUnit)unit withSize:(NSSize)preferredSize {
    (void)preferredSize;  // The editor has a size of its own (View > Window size)
    std::shared_ptr<AuInstance>* instance = nullptr;
    UInt32 size = sizeof(instance);
    if (AudioUnitGetProperty(unit, kInstanceProperty, kAudioUnitScope_Global, 0, &instance, &size) != noErr || instance == nullptr ||
        !*instance || !(*instance)->editor) {
        return nil;
    }
    return [[D110EmuAUView alloc] initWithInstance:*instance];
}

@end
