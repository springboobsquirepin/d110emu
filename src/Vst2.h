#pragma once

// The VST 2.4 plugin interface, as far as D110Emu's plugin uses it. Steinberg no longer distributes the VST 2 SDK, so
// these declarations are written from the interface's public descriptions (the structure layouts and numbers that
// open-source hosts and plugins rely on), not taken from the SDK. The names follow the SDK's, so that descriptions of
// it apply. VST is a trademark of Steinberg Media Technologies GmbH.

#include <cstdint>

#if defined(_WIN32)
#define VST2_CALL __cdecl
#else
#define VST2_CALL
#endif

namespace vst2 {

struct AEffect;

// The host's function, which the plugin calls with HostOpcode requests.
using HostCallback = intptr_t(VST2_CALL*)(AEffect* effect, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt);
using DispatcherProc = intptr_t(VST2_CALL*)(AEffect* effect, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt);
using ProcessProc = void(VST2_CALL*)(AEffect* effect, float** inputs, float** outputs, int32_t sampleFrames);
using ProcessDoubleProc = void(VST2_CALL*)(AEffect* effect, double** inputs, double** outputs, int32_t sampleFrames);
using SetParameterProc = void(VST2_CALL*)(AEffect* effect, int32_t index, float value);
using GetParameterProc = float(VST2_CALL*)(AEffect* effect, int32_t index);

constexpr int32_t kEffectMagic = int32_t((uint32_t('V') << 24) | (uint32_t('s') << 16) | (uint32_t('t') << 8) | uint32_t('P'));
constexpr int32_t kVstVersion = 2400;  // VST 2.4

// 8-byte packing, as the interface is declared; every member is naturally aligned, so this is also the compilers'
// default layout on 64-bit Windows, Linux and macOS.
#pragma pack(push, 8)

// The plugin's description of itself: filled by the plugin, read (and partly written) by the host.
struct AEffect {
    int32_t magic;  // kEffectMagic
    DispatcherProc dispatcher;
    ProcessProc process;  // Deprecated: adds to the outputs
    SetParameterProc setParameter;
    GetParameterProc getParameter;
    int32_t numPrograms;
    int32_t numParams;
    int32_t numInputs;
    int32_t numOutputs;
    int32_t flags;  // EffectFlags
    intptr_t resvd1;  // The host's
    intptr_t resvd2;
    int32_t initialDelay;  // Latency in samples
    int32_t realQualities;  // Deprecated
    int32_t offQualities;   // Deprecated
    float ioRatio;          // Deprecated
    void* object;  // The plugin's own object
    void* user;    // The host's
    int32_t uniqueID;
    int32_t version;
    ProcessProc processReplacing;
    ProcessDoubleProc processDoubleReplacing;
    char future[56];
};

// An event for effProcessEvents; `type` says which kind.
struct VstEvent {
    int32_t type;
    int32_t byteSize;     // Of the whole event, less the first two fields
    int32_t deltaFrames;  // Offset into the block about to be processed
    int32_t flags;
    char data[16];
};

struct VstMidiEvent {
    int32_t type;  // kVstMidiType
    int32_t byteSize;
    int32_t deltaFrames;
    int32_t flags;
    int32_t noteLength;  // In frames, 0 if unknown
    int32_t noteOffset;
    char midiData[4];  // Status and data bytes; the fourth is zero
    char detune;
    char noteOffVelocity;
    char reserved1;
    char reserved2;
};

struct VstMidiSysexEvent {
    int32_t type;  // kVstSysExType
    int32_t byteSize;
    int32_t deltaFrames;
    int32_t flags;
    int32_t dumpBytes;  // Length of sysexDump
    intptr_t resvd1;
    char* sysexDump;  // F0 ... F7
    intptr_t resvd2;
};

// The events of a block: numEvents pointers (the array is as long as that).
struct VstEvents {
    int32_t numEvents;
    intptr_t reserved;
    VstEvent* events[2];
};

// The host's transport (audioMasterGetTime); `flags` says which fields are valid.
struct VstTimeInfo {
    double samplePos;
    double sampleRate;
    double nanoSeconds;
    double ppqPos;  // Quarter notes
    double tempo;   // BPM
    double barStartPos;
    double cycleStartPos;
    double cycleEndPos;
    int32_t timeSigNumerator;
    int32_t timeSigDenominator;
    int32_t smpteOffset;
    int32_t smpteFrameRate;
    int32_t samplesToNextClock;
    int32_t flags;
};

// An output's (or input's) description, for effGetOutputProperties.
struct VstPinProperties {
    char label[64];
    int32_t flags;  // PinFlags
    int32_t arrangementType;
    char shortLabel[8];
    char future[48];
};

// The editor's size, for effEditGetRect.
struct ERect {
    int16_t top;
    int16_t left;
    int16_t bottom;
    int16_t right;
};

#pragma pack(pop)

// Requests from the host to the plugin: AEffect::dispatcher(effect, opcode, index, value, ptr, opt).
enum EffectOpcode : int32_t {
    effOpen = 0,
    effClose = 1,              // The plugin deletes itself
    effSetProgram = 2,         // value: program
    effGetProgram = 3,
    effSetProgramName = 4,     // ptr: char*
    effGetProgramName = 5,     // ptr: char[kVstMaxProgNameLen]
    effGetParamLabel = 6,      // index, ptr: char[kVstMaxParamStrLen]
    effGetParamDisplay = 7,
    effGetParamName = 8,
    effSetSampleRate = 10,     // opt: sample rate
    effSetBlockSize = 11,      // value: largest block
    effMainsChanged = 12,      // value: 1 = resume, 0 = suspend
    effEditGetRect = 13,       // ptr: ERect**
    effEditOpen = 14,          // ptr: the parent window (HWND on Windows)
    effEditClose = 15,
    effEditIdle = 19,
    effGetChunk = 23,          // index: 1 = one program, 0 = the bank; ptr: void** for the data; returns its size
    effSetChunk = 24,          // index as effGetChunk, value: size, ptr: the data
    effProcessEvents = 25,     // ptr: VstEvents*, for the next block
    effCanBeAutomated = 26,
    effGetProgramNameIndexed = 29,  // index: program, ptr: char[kVstMaxProgNameLen]
    effGetInputProperties = 33,     // index: input, ptr: VstPinProperties*
    effGetOutputProperties = 34,    // index: output, ptr: VstPinProperties*
    effGetPlugCategory = 35,
    effSetBypass = 44,
    effGetEffectName = 45,     // ptr: char[kVstMaxEffectNameLen]
    effGetVendorString = 47,   // ptr: char[kVstMaxVendorStrLen]
    effGetProductString = 48,  // ptr: char[kVstMaxProductStrLen]
    effGetVendorVersion = 49,
    effVendorSpecific = 50,
    effCanDo = 51,             // ptr: const char*; returns 1 yes, -1 no, 0 don't know
    effGetTailSize = 52,
    effGetParameterProperties = 56,
    effGetVstVersion = 58,
    effEditKeyDown = 59,       // index: character, value: VirtualKey, opt: ModifierKey flags; returns 1 if used
    effEditKeyUp = 60,
    effStartProcess = 71,
    effStopProcess = 72,
    effSetProcessPrecision = 77,
    effGetNumMidiInputChannels = 78,
    effGetNumMidiOutputChannels = 79,
};

// Requests from the plugin to the host: HostCallback(effect, opcode, index, value, ptr, opt).
enum HostOpcode : int32_t {
    audioMasterAutomate = 0,
    audioMasterVersion = 1,
    audioMasterIdle = 3,
    audioMasterGetTime = 7,          // value: wanted flags; returns VstTimeInfo*
    audioMasterIOChanged = 13,
    audioMasterSizeWindow = 15,      // index: width, value: height; returns 1 if the host resized its window
    audioMasterGetSampleRate = 16,
    audioMasterGetBlockSize = 17,
    audioMasterGetVendorString = 32,
    audioMasterGetProductString = 33,
    audioMasterCanDo = 37,           // ptr: const char*, e.g. "sizeWindow"
    audioMasterUpdateDisplay = 42,
};

enum EffectFlags : int32_t {
    effFlagsHasEditor = 1 << 0,
    effFlagsCanReplacing = 1 << 4,
    effFlagsProgramChunks = 1 << 5,  // The state goes through effGetChunk/effSetChunk
    effFlagsIsSynth = 1 << 8,
    effFlagsNoSoundInStop = 1 << 9,
    effFlagsCanDoubleReplacing = 1 << 12,
};

enum EventType : int32_t {
    kVstMidiType = 1,
    kVstSysExType = 6,
};

enum PlugCategory : int32_t {
    kPlugCategUnknown = 0,
    kPlugCategEffect = 1,
    kPlugCategSynth = 2,
};

enum PinFlags : int32_t {
    kVstPinIsActive = 1 << 0,
    kVstPinIsStereo = 1 << 1,  // This pin and the next one are a stereo pair
    kVstPinUseSpeaker = 1 << 2,
};

enum TimeInfoFlags : int32_t {
    kVstTransportChanged = 1 << 0,
    kVstTransportPlaying = 1 << 1,
    kVstTransportCycleActive = 1 << 2,
    kVstTransportRecording = 1 << 3,
    kVstNanosValid = 1 << 8,
    kVstPpqPosValid = 1 << 9,
    kVstTempoValid = 1 << 10,
    kVstBarsValid = 1 << 11,
    kVstCyclePosValid = 1 << 12,
    kVstTimeSigValid = 1 << 13,
};

// String buffer sizes, including the terminating zero.
enum StringLength : int32_t {
    kVstMaxProgNameLen = 24,
    kVstMaxParamStrLen = 8,
    kVstMaxEffectNameLen = 32,
    kVstMaxVendorStrLen = 64,
    kVstMaxProductStrLen = 64,
};

// Keys for effEditKeyDown/effEditKeyUp (`value`); printable keys come as their character (`index`) with value 0.
enum VirtualKey : int32_t {
    VKEY_BACK = 1,
    VKEY_TAB = 2,
    VKEY_CLEAR = 3,
    VKEY_RETURN = 4,
    VKEY_PAUSE = 5,
    VKEY_ESCAPE = 6,
    VKEY_SPACE = 7,
    VKEY_NEXT = 8,
    VKEY_END = 9,
    VKEY_HOME = 10,
    VKEY_LEFT = 11,
    VKEY_UP = 12,
    VKEY_RIGHT = 13,
    VKEY_DOWN = 14,
    VKEY_PAGEUP = 15,
    VKEY_PAGEDOWN = 16,
    VKEY_SELECT = 17,
    VKEY_PRINT = 18,
    VKEY_ENTER = 19,
    VKEY_SNAPSHOT = 20,
    VKEY_INSERT = 21,
    VKEY_DELETE = 22,
    VKEY_HELP = 23,
    VKEY_NUMPAD0 = 24,  // ... VKEY_NUMPAD9 = 33
    VKEY_MULTIPLY = 34,
    VKEY_ADD = 35,
    VKEY_SEPARATOR = 36,
    VKEY_SUBTRACT = 37,
    VKEY_DECIMAL = 38,
    VKEY_DIVIDE = 39,
    VKEY_F1 = 40,  // ... VKEY_F12 = 51
    VKEY_NUMLOCK = 52,
    VKEY_SCROLL = 53,
    VKEY_SHIFT = 54,
    VKEY_CONTROL = 55,
    VKEY_ALT = 56,
    VKEY_EQUALS = 57,
};

// Modifier flags for effEditKeyDown/effEditKeyUp (`opt`).
enum ModifierKey : int32_t {
    MODIFIER_SHIFT = 1 << 0,
    MODIFIER_ALTERNATE = 1 << 1,  // Alt
    MODIFIER_COMMAND = 1 << 2,    // Control on the Mac
    MODIFIER_CONTROL = 1 << 3,    // Ctrl on Windows
};

}  // namespace vst2
