#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "D20Rhythm.h"
#include "MidiInput.h"
#include "MultiOutputConverter.h"
#include "MidiPlayer.h"
#include "Mt32Translator.h"
#include "mt32emu.h"
#include "DSeriesReverb.h"

constexpr int kBasePartCount = 9;   // Parts 1-8 plus the rhythm part
constexpr int kMaxPartCount = 16;   // 16-part mode: parts 9-15 as well (15 melodic parts and rhythm)
constexpr int kRhythmPart = 8;      // Part numbers 0-7 are parts 1-8, 9-15 are parts 9-15
constexpr int kDefaultPartials = 32;  // The D-110 has 32 partials, like the MT-32
constexpr int kMaxPartials = 512;     // Upper limit for the extended polyphony option
constexpr uint8_t kChannelOff = 16;

// Offsets within a part's timbre temporary area (03 00 00 + 10H * part); see the D-110 MIDI implementation.
namespace TimbreTemp {
constexpr uint8_t ToneGroup = 0x00, ToneNumber = 0x01, KeyShift = 0x02, FineTune = 0x03, BenderRange = 0x04,
                  AssignMode = 0x05, OutputAssign = 0x06, OutputLevel = 0x08, Panpot = 0x09, KeyRangeLower = 0x0A,
                  KeyRangeUpper = 0x0B;
}

// Position of a panpot (0-14, left to right) on the fine pan scale (-64..+64).
inline int panpotToFinePan(int panpot) {
    const int p = (panpot < 0 ? 0 : panpot > 14 ? 14 : panpot) - 7;
    return p >= 0 ? (p * 64 + 3) / 7 : -((-p * 64 + 3) / 7);
}

// Which memory areas a SysEx dump contains.
enum DumpContents : unsigned {
    DumpTones = 1 << 0,      // Tone memory i01-i64
    DumpTimbres = 1 << 1,    // Timbre memory A11-B88
    DumpPatches = 1 << 2,    // Patch memory I-11..I-88 (D-110)
    DumpRhythm = 1 << 3,     // Rhythm setup
    DumpSystem = 1 << 4,     // System area
    DumpTemporary = 1 << 5,  // Parts' current timbres and tones
    DumpD20Patches = 1 << 6, // D-20 performance patches, when present
    DumpPerformance = 1 << 7, // The current D-20 performance patch (03 04 00)
    DumpCard = 1 << 8,        // The memory card: tones c11-c88, timbres C-A11-C-B88, patches C-11-C-88 (not in DumpEverything)
    DumpD20Rhythm = 1 << 9,   // D-20 rhythm patterns (P-51-P-88, and P-11-P-48 at 0D 00 00) and rhythm track, when present
    DumpMemory = DumpTones | DumpTimbres | DumpPatches | DumpRhythm | DumpSystem | DumpD20Patches | DumpD20Rhythm,
    DumpEverything = DumpMemory | DumpTemporary | DumpPerformance,
};

// Analog output emulation. Auto selects the best mode for the output sample rate.
enum class AnalogMode : int { Auto = -1, DigitalOnly = 0, Coarse = 1, Accurate = 2, Oversampled = 3 };

// Everything that requires recreating the synth when changed.
struct EngineConfig {
    std::filesystem::path controlRom;
    std::filesystem::path pcmRom;
    AnalogMode analogMode = AnalogMode::Auto;
    uint32_t outputSampleRate = 48000;
    MT32Emu::SamplerateConversionQuality resamplerQuality = MT32Emu::SamplerateConversionQuality_GOOD;
    uint32_t partialCount = kDefaultPartials;
    bool sixteenParts = false;  // D-110 only: parts 9-15 besides parts 1-8 and rhythm
    // Each part's own stereo output besides the mix, for renderBuses() (the plugins): see setPartOutputMask().
    bool partOutputs = false;
    // The MULTI outputs 1-6 as outputs of their own, for renderBuses() (the plugins; the standalone's 7.1 surround): notes
    // on MULTI 1-6 play there rather than in the mix (MT32Emu::Synth::setMultiOutputsEnabled).
    bool multiOutputs = false;
};

// Settings applied to the running synth immediately; they survive reconfiguration.
struct EngineOptions {
    float outputGain = 1.0f;
    float reverbGain = 1.0f;
    bool reverbEnabled = true;
    bool reverbOverridden = false;  // Ignore reverb changes sent over MIDI
    bool reversedStereo = false;
    bool niceAmpRamp = true;
    bool nicePanning = false;
    bool nicePartialMixing = false;
    MT32Emu::DACInputMode dacInputMode = MT32Emu::DACInputMode_NICE;
    bool midiTimestamping = true;   // Trade one audio block of latency for jitter-free live MIDI
    // Space MIDI messages at the speed of a MIDI cable (about 0.8 ms per note), as a real unit receives them.
    // Off: messages play as fast as they arrive, which suits USB and virtual ports.
    bool midiCableSpeed = false;
    uint8_t controlChannel = kChannelOff;  // D-110 System Setup: program changes here select patches
    uint8_t unitNumber = 17;                // D-110 System Setup: SysEx unit number 17-32
    // Beyond the original MIDI implementation: filter NRPNs and CC 74/71, tuning RPNs, GM/GS/XG reset.
    bool midiExtensions = true;
    // D-110 ROMs: reverb types 1-8 through the tunable D-series model (setDSeriesReverbSettings); off, through the MT-32
    // family's reverb models as mt32emu has them.
    bool dSeriesReverb = true;
};

struct PartStatus {
    char name[11] = {};   // Tone name
    char tone[4] = {};    // D-110 tone number such as "a11" (group a/b/i/r, bank 1-8, number 1-8)
    uint8_t temp[16] = {}; // Raw timbre temporary area, see TimbreTemp offsets
    int finePan = 0;       // -64..+64 (left to right): the fine pan, or the panpot's position when there is none
    bool finePanSet = false;
    uint8_t channel = kChannelOff;
    uint8_t reserve = 0;   // Partial reserve setting (out of 32)
    uint32_t reservedPartials = 0;  // The reserve in partials, scaled to the partial count
    uint32_t activePartials = 0;    // Partials the part is playing
    uint8_t program = 0xFF; // Timbre (0-127, A11-B88) from the last program change, 0xFF = none
    bool programFromCard = false;  // The timbre is C-A11-C-B88 of the memory card
    bool programFromAlt = false;   // The timbre is one of D11-E88 (tones d11-e88 with a new timbre's settings)
    uint8_t midiVolume = 100;
    uint8_t expression = 100;
    uint32_t bendRangeCents = 0;  // The range pitch bend spans each way: the channel's (EngineStatus::midiExtensions) or the timbre's
    bool active = false;  // At least one non-releasing partial
    uint32_t noteCount = 0;
    uint8_t keys[kMaxPartials] = {};
    uint8_t velocities[kMaxPartials] = {};
};

struct PlayerStatus {
    MidiPlayer::State state = MidiPlayer::State::Empty;
    std::string name;
    std::string queuedName;  // The file queued to follow (playerQueueNext), empty if none
    double position = 0.0;
    double duration = 0.0;
    bool loop = false;
};

struct EngineStatus {
    bool open = false;
    std::string controlRomName;
    std::string pcmRomName;
    AnalogMode analogMode = AnalogMode::Coarse;  // Mode in use (never Auto)
    uint32_t outputSampleRate = 0;
    PartStatus parts[kMaxPartCount];  // Indexed by part number; see partCount
    uint32_t partCount = kBasePartCount;  // 9, or 16 in 16-part mode
    uint32_t partialCount = 0;
    std::vector<MT32Emu::PartialState> partialStates;
    uint8_t masterVolume = 0;
    // System area 00: A4 = 440 Hz x 2^((value - 64) / 1536); 40H = 440 Hz (the D-110's default), 4AH = 442 Hz (the MT-32's)
    uint8_t masterTune = 0x40;
    int gsMasterTune = 0;       // MIDI extensions: a GS master tune received, in tenths of a cent, on top of masterTune
    // The MIDI extensions are in effect: switched on, and not off for music made for the units (MT-32 translation, an
    // MT-32's own ROMs, a ROM Play song).
    bool midiExtensions = false;
    uint8_t reverbMode = 0;   // D-110: 0-8 (Small/Medium Room, Medium/Large Hall, Plate, Delay 1/2/3, OFF); MT-32: 0-3
    uint8_t reverbTime = 0;
    uint8_t reverbLevel = 0;
    bool d110 = false;        // D-110 address map and behaviour
    uint8_t currentPatch = 0; // D-110 patch I-11..I-88 as 0-63
    char patchName[11] = {};
    // D-20 performance mode: parts 1 (upper) and 2 (lower) play the current performance patch.
    bool performanceMode = false;
    bool cardInserted = false;
    bool mt32Translation = false;  // MIDI from outside is translated from the MT-32 (D-110 ROMs only)
    bool mt32PresetBanks = false;  // ...and the D-110's preset tones hold the MT-32's presets as chosen, d and e its own
    bool altTones = false;         // Tone banks d and e (timbres D11-E88) are there: the MT-32's presets, or the D-110's
    std::string mt32PresetRom;     // Description of the MT-32 control ROM the presets come from; empty without one
    uint8_t performanceChannel = 0;
    uint8_t currentPerformance = 0;  // A11-B88 as 0-127
    uint8_t performanceTemp[38] = {};  // The patch temporary area (03 04 00), laid out like D20PatchParam
    std::string lcdMessage;   // Text written to the display area over SysEx (32 characters on the D-110)
    bool lcdMessageShown = false;  // Until a display reset or the user takes over the display
    uint32_t lcdMessageSerial = 0; // Increments with every display write
    uint64_t midiEvents = 0;
    uint64_t queueOverflows = 0;
    PlayerStatus player;
};

// Owns the emulated synth and serialises all access to it.
//
// Threads: the audio thread calls render(); MIDI driver threads call the MidiInputSink methods;
// the UI thread calls everything else. One mutex guards the synth. Reconfiguration builds the
// new synth outside the lock and only swaps pointers under it, so audio never waits on ROM loading.
// What a MIDI file for the player is: music from outside, which MT-32 translation and the MIDI extensions apply to;
// one of the unit's own songs (ROM Play), never translated and played with the MIDI extensions off, as the unit plays
// it; or D-20 rhythm (a pattern or the track), never translated.
enum class MidiFileKind { External, UnitSong, UnitRhythm };

class SynthEngine : public MidiInputSink, private MT32Emu::ReportHandler {
public:
    using Clock = std::chrono::steady_clock;

    SynthEngine();
    ~SynthEngine() override;
    SynthEngine(const SynthEngine&) = delete;
    SynthEngine& operator=(const SynthEngine&) = delete;

    // Creates (or recreates) the synth. The previous synth keeps running if this fails.
    // Memory (tones, timbres, patches, rhythm setup, system) is carried over from the previous synth
    // when both use the D-110 address map, as the real unit's battery keeps it across power cycles.
    bool configure(const EngineConfig& config, std::string& error, bool preserveMemory = true);
    void close();
    bool isOpen() const;

    // A MIDI event at a sample offset into the block render() is about to fill (from a plugin host).
    struct TimedMidi {
        uint32_t frame;         // Output frames into the block
        uint32_t shortMessage;  // 0 for SysEx
        uint32_t sysexOffset;   // Into the SysEx bytes passed with the events
        uint32_t sysexLength;
    };

    // Audio thread: fills interleaved stereo float frames at the configured output rate. `events` (a plugin host's MIDI
    // for this block, in order of frame) play at their own frames: the block is rendered in pieces between them. They
    // go through MT-32 translation when it is on, like live MIDI, which then plays at the start of the block.
    void render(float* interleavedStereo, uint32_t frames, const TimedMidi* events = nullptr, size_t eventCount = 0,
                const uint8_t* sysex = nullptr);
    // As render(), with each part's own output (EngineConfig::partOutputs) into parts[part number]: kPartOutputs
    // entries, parts 1-8 (0-7), rhythm (8) and parts 9-15 (9-15); and the MULTI outputs (EngineConfig::multiOutputs)
    // into multi: kMultiPairs interleaved stereo pairs, MULTI 1 and 2, 3 and 4, 5 and 6. Any entry, or array, may be
    // null; silent where the configuration has no such outputs.
    static constexpr int kPartOutputs = MultiOutputConverter::kParts;
    static constexpr int kMultiPairs = MultiOutputConverter::kMultiPairs;
    void renderBuses(float* mix, float* const* parts, float* const* multi, uint32_t frames, const TimedMidi* events = nullptr,
                     size_t eventCount = 0, const uint8_t* sysex = nullptr);
    // The standalone's 7.1 surround (EngineConfig::multiOutputs): `frames` frames of AudioOutput's eight channels, front
    // left and right = the mix + MULTI 1 and 2, centre and LFE silent, back (rear) left and right = MULTI 3 and 4, side
    // left and right = MULTI 5 and 6. Without MULTI outputs those are silent and the mix plays in front.
    static constexpr int kSurroundChannels = 8;
    void renderSurround(float* interleaved, uint32_t frames);
    // Part outputs: the parts whose own output is in use (bit n = part number n). All their notes play there, with their
    // pan, rather than in the mix: dry, those on Mix + reverb also feeding the reverb, whose return stays in the mix; see
    // MT32Emu::Synth::setPartOutputsAvailable. Kept across restarts.
    void setPartOutputMask(uint32_t mask);
    uint32_t partOutputMask() const;
    // With MULTI outputs (EngineConfig::multiOutputs): MULTI 1+2, 3+4 and 5+6 as three stereo outputs, where notes on either
    // output of a pair play with their pan, instead of six mono ones (MT32Emu::Synth::setMultiPairsStereo). Kept across
    // restarts.
    void setMultiPairsStereo(bool stereo);
    bool multiPairsStereo() const;

    // MidiInputSink: live MIDI from any thread.
    void onMidiShortMessage(uint32_t message) override;
    void onMidiSysex(const uint8_t* data, size_t length) override;

    void allNotesOff();
    // Reset MIDI: silences all parts, resets what the MIDI channels carry (controllers, pedal, bend, RPN/NRPN, what the
    // MIDI extensions set) and then every part's level and pan (resetPartMix), as a GM or GS reset does.
    void resetMidiChannels();
    // D-110 ROMs: every part's level to 100 and pan to the centre, the rhythm keys' own left alone (Reset MIDI, and
    // Restart synth from the interface). While translating it goes through the translation as the MT-32 side's own
    // writes, so the translation keeps them (it sends the parts again when the presets change).
    void resetPartMix();
    // These write the synth's system area with Roland DT1 SysEx, as a MIDI controller would.
    // MIDI channels by part number (entries beyond the synth's part count are ignored).
    void setPartChannels(const std::array<uint8_t, kMaxPartCount>& channels);
    void setMasterVolume(int volume);
    void setReverb(int mode, int time, int level);
    void setMasterTune(int value);  // 0-127, into the system area like a SysEx write
    // Solo and mute: bit n = part number n (8 = rhythm) is not heard; kept for every synth configured later.
    void setMutedParts(uint32_t partMask);
    // The D-series reverb's parameters (EngineOptions::dSeriesReverb): at once, and for every synth configured later.
    void setDSeriesReverbSettings(const MT32Emu::DSeriesReverbSettings& settings);
    MT32Emu::DSeriesReverbSettings dSeriesReverbSettings() const;

    void setOptions(const EngineOptions& options);
    EngineOptions options() const;

    // SysEx files: every message is applied at once, like a bulk load (D-20 rhythm patterns and track included).
    // With MT-32 translation on, files with MT-32 data are translated unless `translate` is false.
    bool loadSysexFile(const std::filesystem::path& path, std::string& error, bool translate = true);
    // Loads the rhythm patterns P-51-P-88 of a D-20 dump as the preset patterns P-11-P-48, which a dump never holds
    // (they are in the D-20's ROM); nothing else of the file is used.
    bool loadD20PresetPatterns(const std::filesystem::path& path, std::string& error);
    // Writes the rhythm patterns P-51-P-88 of a D-20 dumped after a factory reset (the patterns its memory starts with)
    // into the slots this memory has never held (all zeros), so patterns loaded or kept are left alone. Nothing else of
    // the file is used.
    bool loadD20InitialPatterns(const std::filesystem::path& path, std::string& error);
    // The D-20 rhythm patterns and rhythm track in memory (D-110 ROMs only; empty otherwise).
    D20Rhythm d20Rhythm();
    // Back to the D-20's factory rhythm track, which the memory starts with: the presets P-11-P-48 in order, each twice.
    void resetD20RhythmTrack();
    // Writes the chosen memory areas as Roland DT1 messages, which a real D-110 also accepts.
    bool saveSysexFile(const std::filesystem::path& path, unsigned contents, std::string& error);
    std::vector<uint8_t> dumpSysex(unsigned contents);
    // Applies SysEx messages at once (e.g. a dump taken with dumpSysex()).
    void applySysex(const std::vector<uint8_t>& data);

    // D-110 panel functions. Addresses are 7-bit SysEx addresses such as 0x030000.
    void recallPatch(int patchNumber);
    // Memory card: a card is a SysEx file of the card areas (18 00 00 tones, 15 00 00 timbres, 16 00 00 patches).
    // insertCard() with an empty path inserts a blank card.
    bool insertCard(const std::filesystem::path& path, std::string& error);
    void ejectCard();
    bool saveCard(const std::filesystem::path& path, std::string& error);
    // The D-110's Save to Card / Load from Card: copies all tones, timbres and patches between memory and the card.
    void copyMemoryToCard();
    void copyCardToMemory();
    // D-20 performance mode (see MT32Emu::Synth::setPerformanceMode); leaving it does not restore the parts.
    void setPerformanceMode(bool enabled, int channel);
    void recallPerformance(int patchNumber);
    void setCurrentPatchNumber(int patchNumber);
    // The D-20 performance patch shown as current (A11-B88 as 0-127), without recalling it (after the memory is restored).
    void setCurrentPerformanceNumber(int patchNumber);
    // MT-32 translation (see Mt32Translator): MIDI input, MIDI files and SysEx files are taken as MT-32 data and
    // converted for the D-110, and SysEx sets the master volume as on the MT-32. Only takes effect with D-110 ROMs.
    // Switching it changes no memory; mt32PowerOn() sets up the MT-32's power-on state (channels 2-10, the
    // MT-32's patch memory, rhythm setup and parts) in the D-110's memory, as an MT-32 reset does.
    void setMt32Translation(bool enabled);
    bool mt32Translation() const;
    void mt32PowerOn();
    // The MT-32's presets (from an MT-32 or CM-32L control ROM; nullptr = none). With them, tone banks d and e
    // (d11-e88, played by the timbres D11-E88) hold the MT-32's A1-B64, translated for the D-110. While translating in
    // Hybrid or Exact mode, the D-110's preset tones hold the MT-32's presets as they play instead (a11-b88 = A1-B64,
    // r01-r63 = R1-R63: the MT-32's own, or a copy of the chosen D-110 stand-in), so MT-32 program changes and rhythm
    // keys select them by number, and d and e hold the D-110's own a and b; they swap back when translation ends.
    // Without the MT-32's presets, or in StandIns mode, D-110 presets of the same kind stand in.
    void setMt32Presets(std::shared_ptr<const Mt32Presets> presets);
    // StandIns, Hybrid (as chosen) or Exact (the default), and the choices (see Mt32Translator).
    void setMt32PresetMode(Mt32Translator::PresetMode mode);
    void setMt32PresetChoices(const Mt32Translator::PresetChoices& presets, const Mt32Translator::RhythmChoices& rhythm);
    // MT-32 toms on the D-110's TomTom1 set (roomier) instead of TomTom2 (the MT-32's sample); see Mt32Translator.
    void setMt32RoomyToms(bool enabled);
    // Timbre 0-127 (A11-B88), 128-255 (C-A11-C-B88) from the memory card, or 256-383 (D11-E88, the tone banks d and e).
    void setPartProgram(int part, int timbreNumber);
    void setPartParameter(int part, uint8_t offset, uint8_t value);
    // Fine pan -64..+64 of a part or of a rhythm key (24-108), heard with nice panning (D-110 ROMs only). It moves
    // the panpot to its nearest step; writing the panpot drops it.
    void setPartFinePan(int part, int pan);
    void setRhythmFinePan(int key, int pan);
    // Writes bytes into a part's timbre temporary area (03 00 00 + 10H * part, or 13 00 00 for parts 9-15).
    void writePartTemp(int part, uint8_t offset, const uint8_t* data, size_t length);
    void writeData(uint32_t sysexAddress, const uint8_t* data, size_t length);
    void readMemory(uint32_t sysexAddress, uint32_t length, uint8_t* out);
    // Names of all 448 tones (a, b, i, r, the card's c, then d and e; groups of 64), empty when the synth is closed.
    std::vector<std::string> toneNames();
    void dismissLcdMessage();

    bool loadMidiFile(const std::filesystem::path& path, std::string& error);
    void loadMidi(std::unique_ptr<SmfFile> file, std::string name, MidiFileKind kind = MidiFileKind::External);
    void playerPlay();
    void playerPause();
    void playerStop();
    void playerSetLoop(bool loop);
    void playerSetSpeed(double speed);  // 1 = as written; resets to 1 when a file is loaded
    // Queues a MIDI file to follow the one playing, from the sample where that one ends (MidiPlayer::queueNext), with
    // the same translation setting; nullptr clears the queue.
    void playerQueueNext(std::unique_ptr<SmfFile> file, std::string name);

    void getStatus(EngineStatus& status);

    // Debug output, errors and display messages from the synth since the last call.
    std::vector<std::string> takeLog();
    void log(const std::string& line);

private:
    // MT32Emu::ReportHandler, called from inside the synth (render thread or configure()).
    void printDebug(const char* fmt, va_list list) override;
    void showLCDMessage(const char* message) override;
    void onDisplayReset() override;
    void onPatchRecalled(MT32Emu::Bit8u patchNum) override;
    void onWriteRequestResult(MT32Emu::Bit8u result) override;
    void onErrorControlROM() override;
    void onErrorPCMROM() override;
    bool onMIDIQueueOverflow() override;
    void onDeviceReset() override;

    // The following require mutex_ to be held.
    uint32_t liveTimestamp(Clock::time_point arrival, uint32_t now) const;
    // `atOnce`: at `now` rather than at liveTimestamp().
    void playPendingMidi(uint32_t now, bool atOnce = false);
    bool translatingLocked() const;
    // Where MIDI from outside comes from: the MIDI file player, or live (the MIDI inputs, a plugin host).
    enum class MidiSource { Player, Live };
    // Queues a message from outside (short message, or SysEx when shortMessage is 0), translated from the MT-32
    // when translation is on and `translate` is set. Returns false when the synth's queue is full.
    // With the MIDI extensions, once a GM, GS or XG reset has come from `source`, its SysEx for the MT-32's map no
    // longer changes the part channels (10 00 0D-15): a GM or GS file carries such SysEx for an MT-32 beside the GS
    // module (often to turn its parts off), not for the unit that plays the file.
    bool playExternalLocked(uint32_t shortMessage, const uint8_t* sysex, uint32_t sysexLength, uint32_t timestamp, bool translate,
                            MidiSource source);
    bool playExternalMessageLocked(uint32_t shortMessage, const uint8_t* sysex, uint32_t sysexLength, uint32_t timestamp, bool translate);
    // Forgets that a source's stream is General MIDI (both sources without one).
    void endGmStreamLocked(int source = -1);
    // The MIDI extensions, as the option says, but off for music made for the units, which plays as they play it: MT-32
    // translation, an MT-32's own ROMs, and a ROM Play song while it plays or pauses. Before each block and on changes.
    void updateMidiExtensionsLocked();
    // Appends what the MT-32 translation adds to its power-on setup: parts 9-15 (16-part mode) off.
    void appendMt32PowerOnExtras(std::vector<uint8_t>& messages);
    // Configures the translator's presets and fills the synth's preset tones and tone banks d and e as the
    // translation, the preset mode and choices and the presets say (see setMt32Presets()). `resend`: while
    // translating, send the patch memory, rhythm setup and parts again, whose tones depend on them.
    void updateMt32PresetsLocked(bool resend);
    void loadPresetBanksLocked();
    void sendDataSet(uint8_t addressHigh, uint8_t addressMid, uint8_t addressLow, const uint8_t* data, size_t length);
    std::vector<uint8_t> dumpSysexLocked(MT32Emu::Synth& synth, unsigned contents);
    void copyAreaLocked(uint32_t fromPacked, uint32_t toPacked, uint32_t entrySize, uint32_t entries, uint32_t entryStride);
    void clearCardLocked();
    size_t applySysexLocked(MT32Emu::Synth& synth, const uint8_t* data, size_t length);
    void resetPartMixLocked();

    mutable std::mutex mutex_;
    std::unique_ptr<MT32Emu::Synth> synth_;
    std::unique_ptr<MT32Emu::SampleRateConverter> converter_;
    std::unique_ptr<MultiOutputConverter> busConverter_;  // Instead of converter_ with part outputs
    // renderSurround()'s mix and MULTI pairs (interleaved stereo), for kSurroundFrames frames at a time; the audio
    // thread's alone.
    static constexpr uint32_t kSurroundFrames = 8192;
    std::vector<float> surroundMix_;
    std::vector<float> surroundMulti_[kMultiPairs];
    uint32_t partOutputMask_ = 0;
    bool multiPairsStereo_ = false;
    EngineConfig config_;
    EngineOptions options_;
    MT32Emu::DSeriesReverbSettings dSeriesReverbSettings_ = MT32Emu::DSeriesReverbSettings::getDefaults();
    uint32_t mutedParts_ = 0;
    AnalogMode analogModeInUse_ = AnalogMode::Coarse;
    std::string controlRomName_;
    std::string pcmRomName_;
    MidiPlayer player_;
    MidiFileKind playerKind_ = MidiFileKind::External;  // The loaded MIDI file's
    // Per MidiSource: a GM, GS or XG reset came from it (see playExternalLocked), and the log said that it kept the part
    // channels. The player's ends when a file loads or stops; both when the synth restarts or the MIDI extensions or
    // the MT-32 translation are switched.
    bool gmStream_[2] = {};
    bool gmStreamLogged_[2] = {};
    std::vector<uint8_t> gmStreamSysex_;  // Scratch: SysEx without the part channels
    uint64_t midiEvents_ = 0;

    Mt32Translator translator_;
    bool mt32Translation_ = false;
    std::shared_ptr<const Mt32Presets> mt32Presets_;
    Mt32Translator::PresetMode mt32PresetMode_ = Mt32Translator::PresetMode::Exact;
    std::vector<uint8_t> translated_;  // Scratch buffers for translated SysEx and the program changes after it
    std::vector<uint32_t> translatedPrograms_;

    // Live MIDI waits here, stamped with its arrival time, until the next render() schedules it. Input
    // threads never wait for the synth: a burst of notes keeps its timing even while a block renders.
    struct PendingMidi {
        Clock::time_point arrival;
        uint32_t shortMessage;  // 0 for SysEx
        uint32_t sysexOffset;
        uint32_t sysexLength;
    };
    std::mutex pendingMutex_;  // Guards pending_ and pendingSysex_ only; never held while taking mutex_
    std::vector<PendingMidi> pending_;
    std::vector<uint8_t> pendingSysex_;
    std::vector<PendingMidi> playing_;  // Swapped with pending_ by the render thread
    std::vector<uint8_t> playingSysex_;

    // Start of the most recent render() call, used to timestamp live MIDI (see liveTimestamp()).
    std::chrono::steady_clock::time_point renderStartTime_;
    uint32_t renderStartSynthTime_ = 0;
    double renderBlockSynthSamples_ = 0.0;

    std::atomic<uint64_t> queueOverflows_{0};

    std::mutex logMutex_;  // Guards log_ and the LCD fields; taken after mutex_ when both are needed
    std::vector<std::string> log_;
    std::string lcdMessage_;
    bool lcdMessageShown_ = false;
    uint32_t lcdMessageSerial_ = 0;
};
