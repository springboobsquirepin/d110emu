#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "AudioOutput.h"
#include "MidiInput.h"
#include "RomLibrary.h"
#include "RomPlay.h"
#include "Settings.h"
#include "SynthEngine.h"

struct AppOptions {
    std::filesystem::path settingsFile;
    // The emulated battery-backed memory, a SysEx dump loaded at startup and saved on exit (empty: none).
    std::filesystem::path memoryFile;
    // Where to look for a "roms" folder (and in their parents) when none is configured yet.
    std::vector<std::filesystem::path> romSearchDirs;
    bool enableAudio = true;      // false: the caller drives engine().render() itself (headless tools, the plugin)
    bool nullAudio = false;       // The audio output is miniaudio's null device: time passes, nothing is heard (tests)
    bool enableMidiInput = true;
    // The D-series reverb's tuning (d110emu-reverb.ini), loaded at startup and saved after changes (empty: none).
    std::filesystem::path reverbSettingsFile;

    // The plugins: the host sends the MIDI and takes the audio, so the configuration window has no audio device or MIDI
    // ports and the File menu no Exit. The settings and the memory come from the host's project and go back there
    // (settingsText(), memoryData()).
    bool plugin = false;
    std::string pluginFormat = "VST2";  // Which plugin, for the About window and messages: "VST2", "VST3" or "AU"
    // The plugins: each part can play out of an output of its own (EngineConfig::partOutputs), chosen per part in the
    // Output menus and kept with the settings (own_outputs); and MULTI 1-6 are outputs of their own
    // (EngineConfig::multiOutputs).
    bool partOutputs = false;
    // The plugins: MULTI 1-6 come out as three stereo pairs (Multi 1+2, 3+4, 5+6), where notes on either output of a pair
    // play with their pan, rather than six mono outputs (AppCore::multiPairsInUse). Fixed for an instance, as hosts set
    // up a plugin's outputs when they add it.
    bool multiOutputPairs = false;
    // The plugins where the configuration window chooses it (VST2, VST3): the choice for instances added from now on,
    // which PluginCore keeps with the computer's settings. Unset: not a choice (the Audio Unit always has pairs).
    std::function<bool()> multiPairsForNew;
    std::function<void(bool)> setMultiPairsForNew;
    // The VST3 plugin: the outputs the host takes (bit n = part number n's own, bit 16 + n = MULTI n + 1), to point out
    // a part on an output the host has switched off. Unset (VST2): not known.
    std::function<uint32_t()> hostOutputs;
    std::string settingsText;       // The settings when there is no settings file ("key = value" lines)
    std::vector<uint8_t> memory;    // The battery-backed memory when there is no memory file (a SysEx dump)
    uint32_t outputSampleRate = 0;  // Without audio output: the rate to render at (0: the sample_rate setting)
    std::function<void()> viewMenu;  // Dear ImGui front-end: draws more View menu items
    std::string aboutExtra;          // Dear ImGui front-end: a line for the About window (the window's toolkit, say)
    std::function<void(const Settings&)> settingsSaved;  // Called after every change of the settings
    std::vector<std::string> startupMessages;            // Lines for the log at the start (files copied over, say)
};

// The standalone's and the terminal version's files in `folder` (d110emu.ini, d110emu-memory.syx, d110emu-reverb.ini),
// and where to look for the ROMs: inside the app (on macOS its Contents/Resources), next to the program and above it,
// in the working directory, in the folder. With `adoptOld`, the files an earlier version kept next to the program (or on
// Linux in ~/.config/D110Emu) are copied into the folder the first time (Platform::adoptOldFiles), and the log says so.
void useStandaloneFolder(AppOptions& options, const std::filesystem::path& folder, bool adoptOld);

// How the unit and the front-ends write its numbers and name its settings.
namespace UnitText {

inline constexpr const char* kAnalogModeNames[] = {"Auto", "Digital only", "Coarse", "Accurate", "Oversampled"};
inline constexpr const char* kQualityNames[] = {"Fastest", "Fast", "Good", "Best"};
inline constexpr const char* kReverbModeNames[] = {"Room", "Hall", "Plate", "Tap delay"};  // The MT-32's, values 0-3
inline constexpr const char* kDacModeNames[] = {"Nice", "Pure", "Generation 1", "Generation 2"};
inline constexpr int kSampleRates[] = {0, 44100, 48000, 88200, 96000};  // 0: the device's default
inline constexpr int kPartialCounts[] = {32, 64, 128, 256, 512};
inline constexpr int kBufferSizes[] = {128, 256, 512, 1024, 2048};
// The D-110's reverb types (its manual p.26; the D-10/D-20 have the same), values 0-8.
inline constexpr const char* kD110ReverbNames[] = {"Small Room", "Medium Room", "Medium Hall", "Large Hall", "Plate",
                                                   "Delay 1", "Delay 2", "Delay 3", "Off"};
inline constexpr const char* kOutputNames[] = {"Mix", "Mix + reverb", "Multi 1", "Multi 2", "Multi 3", "Multi 4", "Multi 5", "Multi 6"};
inline constexpr const char* kOutputShortNames[] = {"Mix", "Mix+Rev", "Multi 1", "Multi 2", "Multi 3", "Multi 4", "Multi 5", "Multi 6"};
// The same where MULTI 1-6 are three stereo pairs (AppCore::multiPairsInUse): MULTI 1 and 2 are one output, and so on.
inline constexpr const char* kOutputPairNames[] = {"Mix", "Mix + reverb", "Multi 1+2", "Multi 1+2", "Multi 3+4", "Multi 3+4", "Multi 5+6", "Multi 5+6"};
inline constexpr const char* kOutputPairShortNames[] = {"Mix", "Mix+Rev", "M1+2", "M1+2", "M3+4", "M3+4", "M5+6", "M5+6"};
inline constexpr const char* kMT32OutputNames[] = {"Dry", "Reverb"};
// Tone groups as the app numbers them: a, b, i, r (the timbre's group), then the card's c and the extra banks d and e.
// The tone names (SynthEngine::toneNames()) are in the same order, 64 per group.
inline constexpr const char* kToneGroups = "abircde";
constexpr int kCardGroup = 4;
constexpr int kAltGroup = 5;  // d; e is 6
constexpr int kToneGroupCount = 7;

std::string noteName(int note);  // "C4" for 60
// Master tune (system area 00) as the units show it: A4 in Hz, 440 Hz at 64 (the D-110's default) and 442 Hz at 74.
std::string masterTuneText(int value);
std::string formatTime(double seconds);   // "1:05"
std::string channelLabel(uint8_t channel);  // "1".."16", "Off"
// "1".."15" for parts, "R" for the rhythm part (part number 8).
std::string partLabel(int part);
// Tone number such as "a11": group, bank 1-8, number 1-8.
std::string toneCode(int group, int number);
// Timbre number such as "I-A11": 0-127 internal (I-A11-I-B88), 128-255 on the memory card (C-A11-C-B88), 256-383 the
// timbres of tone banks d and e (P-D11-P-E88).
std::string timbreCode(int timbre);
// D-20 performance patch number such as "A11" (A11-B88 = 0-127).
std::string performanceCode(int patch);
// Patch number such as "I-11": 0-63 internal (I-11..I-88), 64-127 on the memory card (C-11..C-88).
std::string patchCode(int patch);
// Pan as the D-110 displays it, left to right: 7> ... 1> >< <1 ... <7.
std::string panLabel(int pan);
std::string padded(const std::string& text, size_t width);  // Cut or filled with spaces to `width`
std::vector<uint8_t> readBinaryFile(const std::filesystem::path& path);

}  // namespace UnitText

// Addresses in the unit's memory (7-bit SysEx addresses such as 0x030110).
namespace UnitMemory {

// 7-bit SysEx address arithmetic (each address byte holds 7 bits).
uint32_t addressPlus(uint32_t sysexAddress, uint32_t offset);

constexpr uint32_t kRhythmSetupAddress = 0x030110;  // + 4 per key from key 24
constexpr uint32_t kRhythmFinePanAddress = 0x120100;  // + 2 per key from key 24 (fine pan, with nice panning)
constexpr uint32_t kReserveAddress = 0x100004;
constexpr uint32_t kExtraReserveAddress = 0x110000;  // 16-part mode: parts 9-15
constexpr uint32_t kPatchNameAddress = 0x100017;
constexpr uint32_t kTimbreWriteAddress = 0x400100;  // + 2 per part
constexpr uint32_t kTimbreMemoryAddress = 0x050000;  // + 8 per timbre
constexpr uint32_t kCardTimbreAddress = 0x150000;    // Memory card timbres, + 8 per timbre
constexpr uint32_t kPerformanceTempAddress = 0x030400;    // D-20 performance patch temporary area (38 bytes)
constexpr uint32_t kPerformanceMemoryAddress = 0x070000;  // + 26H per performance patch
constexpr uint32_t kPatchWriteAddress = 0x400200;

}  // namespace UnitMemory

// The emulator without its interface: the engine, the audio output, the MIDI inputs, the settings and ROMs, the
// battery-backed memory, the unit's modes (D-20 performance mode, MT-32 translation, ROM Play, the D-20 rhythm
// machine) and what its LCD shows. The front-ends derive from it: App (Dear ImGui: the standalone and the plugins) and
// TuiApp (the terminal). It calls no interface code, so it runs without a window.
class AppCore {
public:
    AppCore();
    virtual ~AppCore();
    AppCore(const AppCore&) = delete;
    AppCore& operator=(const AppCore&) = delete;

    // Loads the settings, finds the ROMs, starts the audio output and the synth (which restores the battery-backed
    // memory of the last session) and opens the MIDI inputs.
    virtual void init(const AppOptions& options);
    // Saves the memory, the card, the settings and the reverb tuning, and closes everything (also on destruction).
    void shutdown();
    // Once per interface frame, before drawing: takes the engine's log and status, and follows the player (ROM Play and
    // D-20 patterns ending, a queued pattern taking over).
    void update();

    // Restart synth, as the user asks for it (the System tab, the Synth menu, the terminal version's Options): a new synth
    // with the memory, then every part's level at 100 and pan centred, as Reset MIDI leaves them
    // (SynthEngine::resetPartMix). Settings that need a new synth call restartSynth(), which keeps them.
    bool resetSynth();

    // Plays a MIDI file, or loads a .syx SysEx file (drag & drop, command line).
    void openFile(const std::filesystem::path& path);
    void openMidiFile(const std::filesystem::path& path);
    void loadSysexFile(const std::filesystem::path& path);

    // For tools and tests: start ROM Play song (0-based, -1 = all songs), play a D-20 rhythm pattern (0-63, P-11 first)
    // or the rhythm track (kD20PatternCount), queue the pattern that follows from the next bar, pick the edited part,
    // show the D-20 performance display's tone screen, read the LCD (32 characters).
    void playRomSong(int song);
    void playD20Rhythm(int what) { playD20(what); }
    void queueD20Pattern(int pattern) {
        d20Selected_ = pattern;  // As a click on the pattern does
        queueD20(pattern);
    }
    void setEditPart(int part);
    void showLcdTones(bool on) { lcdToneView_ = on; }
    std::string lcdText() const;
    SynthEngine& engine() { return *engine_; }

    // The plugin: the host's sample rate (the synth restarts when it changes), and what the host's project keeps: all
    // settings as "key = value" lines, and the battery-backed memory as a SysEx dump (as the memory file holds it).
    void setOutputSampleRate(uint32_t rate);
    std::string settingsText();
    std::vector<uint8_t> memoryData();

    // The audio output's highest levels since the last call (1 = full scale), left and right; 7.1 surround's left and
    // right speakers each. The audio callbacks measure them (and tools that render the engine themselves: `samples` in
    // AudioOutput's layout, stereo or 7.1).
    void takeOutputPeaks(float& left, float& right);
    void measurePeaks(const float* samples, uint32_t frames, bool surround);

protected:
    // The time in seconds for the display's timers (the boot message, a queued pattern's blink) and the memory caches: a
    // steady clock here; App takes Dear ImGui's, which the tools advance frame by frame.
    virtual double now() const;
    // The front-end's own settings, read after the core's and written before the settings are saved.
    virtual void loadFrontEndSettings() {}
    virtual void saveFrontEndSettings() {}
    virtual void onLogAdded(const std::string& line) { (void)line; }

    void loadSettings();
    void saveSettings();

    void scanRoms();
    void startAudioAndSynth();
    bool restartSynth();
    EngineConfig engineConfig() const;  // The chosen ROMs and settings, for configure()
    // MULTI 1+2, 3+4 and 5+6 as stereo pairs where the MULTI outputs are outputs of their own (a plugin's, the
    // standalone's 7.1 speakers): notes on either output of a pair play there in stereo, with their pan.
    bool multiPairsInUse() const { return multiPairs_ && (options_.partOutputs || surround_); }
    void setMultiPairs(bool pairs);  // The standalone's choice, at once; a plugin instance's is fixed
    // An output assign's name (0-7: Mix, Mix + reverb, Multi 1-6), or its pair's while the pairs are in use.
    const char* outputName(int assign, bool compact) const;
    uint32_t outputRate() const;
    void refreshMidiPorts();
    void setMidiPortEnabled(const std::string& name, bool enabled);
    void setChannels(const std::array<uint8_t, kMaxPartCount>& channels, bool remember);
    std::array<uint8_t, kMaxPartCount> defaultChannels() const;
    // Part numbers in display order: 1-8, 9-15 (16-part mode), then rhythm.
    std::vector<int> partOrder() const;
    void setOptions();
    void sendShort(uint8_t status, uint8_t data1, uint8_t data2);
    void addLog(const std::string& line);
    void initializeMemory();
    // Keeps the battery-backed memory (D-110 ROMs only) in battery_ and in the memory file, if there is one.
    void saveMemoryFile();
    void saveCardFile();
    // What keeps the emulator from sounding or hearing MIDI (no synth, no audio, a MIDI port that did not open), empty
    // when all is well; `hint` (where the front-end sets them up) follows the synth's and the audio's.
    std::string setupProblem(const std::string& hint) const;
    void applyPartMutes();
    void loadReverbSettingsFile();
    void saveReverbSettingsFile();

    // The display, as the panel's buttons work it: a click returns it from a SysEx message or the boot message, and a
    // change of what it shows (another part, the patch) leaves a SysEx message.
    void dismissLcdMessage();
    void lcdViewChanged();
    // D-20 performance mode: entering keeps a snapshot of the multi-timbral setup, leaving restores it.
    void setPerformanceMode(bool enabled);
    // MT-32 translation: entering keeps a snapshot of what MT-32 data can change (tones, timbres, rhythm setup, system,
    // parts) and sets up the MT-32's power-on state; leaving restores the snapshot.
    void setMt32Mode(bool enabled);
    // D-20 rhythm patterns and rhythm track: play a pattern (0-63, looping) or the track (kD20PatternCount).
    void playD20(int what);
    void stopD20();
    void queueD20(int pattern);  // While a pattern repeats: plays `pattern` from the next bar (the playing one: cancels)
    // The D-20's preset patterns P-11-P-48 are in its ROM, so they come from a file in the ROM folder (kD20PresetFile,
    // recorded from a D-20 with PatternCapture), loaded whenever the synth starts or the memory is initialized. So do
    // the patterns a D-20's memory starts with in P-51-P-88 (kD20InitialFile, a D-20 dumped after a factory reset),
    // written where the memory has never held a pattern.
    void loadD20RomFiles();
    // A timbre's tone group (bytes 00 and 07 of a timbre) in UnitText's numbering: i with the card flag (or of a card
    // timbre) is c, a and b with the alt flag are d and e.
    int toneGroupOf(const uint8_t* timbre, bool cardTimbre) const;
    // Timbres 0-127 (I-A11-I-B88), with a card 128-255 (C-A11-C-B88), with tone banks d and e 256-383 (P-D11-P-E88,
    // their tones with a new timbre's settings): "I-A11  a11 AcouPiano1".
    std::string timbreLabel(int timbre) const;
    int timbreCount() const;  // Memory timbres: 128, or 256 with a card
    bool timbreSelectable(int timbre) const;
    // The part's timbre number in that numbering, -1 if none.
    static int partTimbre(const PartStatus& part);
    void refreshMemoryCaches(bool force);
    void loadRomSongList();
    // Gives the engine the presets of the chosen MT-32 control ROM (mt32PresetRom_), for MT-32 translation and tone
    // banks d and e, with the preset mode and choices.
    void useMt32PresetRom();
    void startRomPlay(int song);
    void restoreAfterRomPlay();

    // By part number: parts 1-8, rhythm, then parts 9-15 (16-part mode: channels 9 and 11-16).
    static constexpr std::array<uint8_t, kMaxPartCount> kD110Channels = {0, 1, 2, 3, 4, 5, 6, 7, 9, 8, 10, 11, 12, 13, 14, 15};
    static constexpr std::array<uint8_t, kMaxPartCount> kMT32Channels = {1, 2, 3, 4, 5, 6, 7, 8, 9, 16, 16, 16, 16, 16, 16, 16};
    static constexpr double kD20BaseTempo = 120.0;  // D-20 patterns are built at this tempo; the player's speed sets the real one
    // The D-20's preset patterns in the ROM folder: a dump with them in P-51-P-88, as PatternCapture saves it.
    static constexpr const char* kD20PresetFile = "D-20 preset patterns.syx";
    // A D-20 dumped after a factory reset (and a power cycle, after which its P-51-P-88 hold the patterns it starts with).
    static constexpr const char* kD20InitialFile = "D-20 initial memory.syx";

    static void renderAudio(void* user, float* interleavedStereo, uint32_t frames);
    static void renderSurroundAudio(void* user, float* interleaved, uint32_t frames);

    AppOptions options_;
    Settings settings_;
    std::unique_ptr<SynthEngine> engine_;
    std::unique_ptr<AudioOutput> audio_;
    std::unique_ptr<MidiInputManager> midi_;
    EngineStatus status_;
    const std::chrono::steady_clock::time_point startTime_ = std::chrono::steady_clock::now();

    // ROMs
    std::filesystem::path romFolder_;
    std::vector<RomEntry> roms_;
    int controlRom_ = -1;
    int pcmRom_ = -1;
    AnalogMode analogMode_ = AnalogMode::Auto;
    int resamplerQuality_ = 2;  // MT32Emu::SamplerateConversionQuality_GOOD
    int partialCount_ = kDefaultPartials;
    bool sixteenParts_ = false;  // 16-part mode (D-110 ROMs only)
    bool performanceMode_ = false;
    int performanceChannel_ = 0;  // 0-15
    std::vector<uint8_t> performanceSnapshot_;  // The multi-timbral setup while in performance mode
    bool mt32Mode_ = false;                     // MT-32 translation (D-110 ROMs only)
    std::vector<uint8_t> mt32Snapshot_;         // The D-110 setup while translating MT-32 data
    int mt32PresetRom_ = -1;                    // MT-32 or CM-32L control ROM in roms_ for exact MT-32 presets
    std::shared_ptr<const Mt32Presets> mt32Presets_;  // ...its presets, nullptr if none
    Mt32Translator::PresetMode mt32PresetMode_ = Mt32Translator::PresetMode::Exact;
    Mt32Translator::PresetChoices mt32PresetChoices_{};
    Mt32Translator::RhythmChoices mt32RhythmChoices_{};
    bool mt32RoomyToms_ = false;
    std::string mt32PresetError_;
    std::vector<uint8_t> battery_;              // The battery-backed memory, kept while other ROMs play
    std::vector<uint8_t> performanceMemory_;    // 128 x 38 bytes (07 00 00)
    std::vector<uint8_t> cardTimbreMemory_;     // 128 x 8 bytes (15 00 00), when a card is inserted
    std::vector<uint8_t> cardPatchMemory_;      // 64 x 128 bytes (16 00 00)
    std::filesystem::path cardFile_;
    std::string synthError_;

    // Audio output
    std::vector<std::string> audioDevices_;
    std::string audioDevice_;  // Empty: system default
    int sampleRate_ = 0;       // 0: device default
    int bufferFrames_ = 512;
    bool surround_ = false;    // 7.1 surround, with MULTI 1-6 on speakers of their own (audio_channels); the standalone's
    // MULTI 1+2, 3+4 and 5+6 as stereo pairs: a plugin instance's layout (AppOptions::multiOutputPairs), else the
    // standalone's choice (multi_output_pairs), heard on its 7.1 speakers.
    bool multiPairs_ = false;
    std::string audioError_;
    std::atomic<float> peakLeft_{0.0f};  // takeOutputPeaks()
    std::atomic<float> peakRight_{0.0f};

    // MIDI input
    std::vector<std::string> midiPorts_;
    std::vector<std::string> enabledMidiPorts_;  // Remembered even while a port is unplugged
    std::string midiError_;
    double nextMidiRefresh_ = 0.0;  // update() looks at the ports every few seconds: inputs plugged in open by themselves

    // Part channels: empty = the ROM's power-on default, else a user choice re-applied after each restart
    bool customChannels_ = false;
    std::array<uint8_t, kMaxPartCount> channels_ = {};

    EngineOptions engineOptions_;
    std::filesystem::path lastMidiFolder_;
    std::string playerError_;
    uint32_t ownOutputs_ = 0;         // The plugins: the parts on their own outputs (bit n = part number n; own_outputs)

    // D-110 panel state
    std::vector<RomSong> romSongs_;
    int romSongChoice_ = 0;   // Index into romSongs_, or romSongs_.size() for the chain of all songs
    int romPlaying_ = -1;     // Song being played (romSongs_.size() = chain), -1 when not in ROM Play
    std::vector<uint8_t> romPlaySnapshot_;  // Setup restored when ROM Play ends
    std::string bootMessage_;
    double bootMessageUntil_ = 0.0;
    bool bootMessagePending_ = false;  // The synth started: the boot message shows from the next update on
    int lcdPart_ = 0;
    bool lcdPatchView_ = false;
    bool lcdToneView_ = false;  // D-20 performance mode: the DISPLAY buttons' screen of the upper and lower tones
    bool lcdPatternPlay_ = false;  // The D-20's Pattern Play screen (while App's Patterns tab shows)
    uint32_t lcdDismissedSerial_ = 0;
    // Mute and solo (by part number); with any solo, the parts without one are not heard.
    std::array<bool, kMaxPartCount> partMuted_{};
    std::array<bool, kMaxPartCount> partSolo_{};
    // The D-series reverb's parameters (Reverb tuning window, d110emu-reverb.ini).
    MT32Emu::DSeriesReverbSettings reverbSettings_ = MT32Emu::DSeriesReverbSettings::getDefaults();
    bool reverbSettingsDirty_ = false; // Saved once no control is held
    int editPart_ = 0;
    std::vector<std::string> toneNames_;
    std::vector<uint8_t> timbreMemory_;  // 128 x 8 bytes (05 00 00)
    std::vector<uint8_t> patchMemory_;   // 64 x 128 bytes (06 00 00)
    std::vector<uint8_t> rhythmSetup_;   // 85 x 4 bytes (03 01 10)
    std::vector<uint8_t> rhythmFinePan_; // 85 x 2 bytes (12 01 00): fine pans of the rhythm keys
    D20Rhythm d20Rhythm_;                // D-20 rhythm patterns and track in memory
    int d20Selected_ = 0;                // P-11
    int d20Playing_ = -1;                // Pattern 0-63 or kD20PatternCount (the track) being played, -1 = none
    int d20Queued_ = -1;                 // Pattern queued to follow the repeating one from its next bar, -1 = none
    std::string d20QueuedTitle_;         // Its MIDI file's title: the player's name once it plays
    double d20QueuedAt_ = 0.0;           // When it was queued (now()): its number blinks on the LCD from then
    bool d20SavedLoop_ = false;          // The player's loop setting before a pattern played (patterns loop)
    std::vector<double> d20BarStarts_;   // Bars of the track being played, in seconds at kD20BaseTempo
    int d20Tempo_ = 120;
    double cacheTime_ = -1.0;

    std::deque<std::string> log_;
};
