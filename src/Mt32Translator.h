#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Mt32Presets.h"

// Converts MIDI meant for a Roland MT-32 (also CM-32L, CM-64, LAPC-I) into what a D-110, D-10 or D-20 needs to sound
// the same, like a translation box between an MT-32 sequencer and the unit. They share the MT-32's SysEx address map
// and tone format; the differences handled here are:
//
//  - Device ID: the MT-32 always listens on 10H (unit #17); its SysEx is readdressed to the unit's number.
//  - PCM waves: the D-series has other samples, in another order. Each MT-32 wave becomes the D-series wave of the
//    same instrument (bank 2 for the MT-32's fixed-pitch drums and loops), often the very same sample. The pitch is
//    corrected to keep the MT-32's: the MT-32 tunes its melodic waves to C4 at coarse pitch C4 and key 60, the
//    D-series tunes them to C2-C6, and shared samples play at other rates.
//  - Presets: MT-32 timbres A1-B64 and rhythm timbres R1-R30 are not the D-series' presets. Without the MT-32's control
//    ROM they become D-series presets of the same kind (stand-ins, see PresetChoice, RhythmChoice and kRhythm; the
//    CM-32L's sound effects mostly fall silent). With it (setPresets()), some or all can be the MT-32's own timbres
//    (PresetMode::Hybrid and Exact): sent to a part's tone as it selects one (for real units), or held by the D-110's
//    preset banks (setPresetBanks(), for d110emu, which fills them).
//  - Pan: panpot 0 is hard right on the MT-32 and hard left on the D-series, so panpots and CC 10 are mirrored.
//  - Reverb: MT-32 Room/Hall/Plate/Tap delay become Small Room/Medium Hall/Plate/Delay 1.
//  - Key ranges: the MT-32 sends dummy zeros where the D-110 keeps a part's key range; they become 0-127.
//  - The display: an MT-32 message (20 characters) is padded to the D-series' 32 so no old text remains.
//  - Reset (7F 00 00), which the D-series ignores, sets up the MT-32's power-on state instead.
//  - Real units (Target, setMemoryInUnit(), setMasterVolumeAsVolume(), setToneCache()): the D-10/D-20's panel
//    channels, memories they cannot take while playing, and no master volume.
//
// SysEx the MT-32 does not have (D-110 patches, write requests, D-20 areas) passes through unchanged.
//
// Some translations depend on bytes other than the ones written (a partial's pitch depends on its wave, whether it
// plays a wave at all on the structure), so the translator keeps a copy of what the MT-32 side has written and
// follows program changes as the MT-32 would. A tone the MT-32 side has not written in full (a part still playing
// a preset, without the MT-32's presets) is translated byte by byte, without pitch correction.
//
// Not thread-safe; SynthEngine and MidiPipe call it with their locks held.
class Mt32Translator {
public:
    struct WaveMapping {
        uint8_t wave;       // D-series wave 0-255 (128-255: bank 2)
        int8_t coarse;      // Semitones added to the partial's coarse pitch
        int8_t fine;        // Cents added to its fine pitch
        uint8_t bank1Wave;  // Bank-1 stand-in (0-127), for tones translated byte by byte
    };
    // Another wave for a partial: where the corrected coarse pitch would leave 0-96 (a few MT-32 loops play their
    // sample far lower or higher than any D-series loop can; usually the same sample, not looped), and the roomy toms.
    struct WaveFallback {
        uint8_t mt32Wave;
        uint8_t wave;
        int8_t coarse;
        int8_t fine;
    };
    static constexpr uint8_t kRhythmOff = 63;  // Rhythm tone r64: "OFF" (the D-20's value 127)

    // The unit the translated MIDI goes to.
    enum class Target {
        D110,  // D-110 (and d110emu): the MT-32's address map; part channels follow the MT-32's through its system area
        D20,   // D-10/D-20 in multi-timbral mode: part channels are set on the panel (setUnitChannels()), so messages
               // move to them; the system area's channels and master volume are dummies, and partial reserves are
               // sent as the package of all 9 the D-20 needs
    };

    // What MT-32 preset timbres (A1-B64) become. The MT-32's own timbres need setPresets(); on a real unit they arrive
    // as the part's tone (one 246-byte message per selection) or from the tone cache (setToneCache()). Rhythm keys
    // keep stand-ins (kRhythm, rhythmChoice()) unless the preset banks hold the MT-32's rhythm timbres.
    enum class PresetMode {
        StandIns,  // D-series presets of the same kind (presetChoice())
        Hybrid,    // Stand-ins, but the MT-32's own timbre where the choice says so (no good D-series counterpart)
        Exact,     // The MT-32's own timbre for every preset
    };

    // What an MT-32 preset becomes.
    struct PresetChoice {
        uint8_t tone = 0;     // Stand-in: group * 64 + number (a11-a88 = 0-63, b11-b88 = 64-127, r01-r64 = 192-255)
        int8_t keyShift = 0;  // Semitones the stand-in is shifted by (added to the patch's key shift)
        bool exact = false;   // Hybrid mode: the MT-32's own timbre instead
        bool operator==(const PresetChoice& other) const {
            return tone == other.tone && keyShift == other.keyShift && exact == other.exact;
        }
    };
    using PresetChoices = std::array<PresetChoice, 128>;
    // What an MT-32 rhythm timbre (R1-R64) becomes.
    struct RhythmChoice {
        int8_t tone = -1;    // Stand-in: rhythm tone r01-r63 as 0-62, kRhythmOff for none; -1 = built in (rhythmTone())
        bool exact = false;  // Hybrid mode: the MT-32's own timbre instead (where the preset banks hold it)
        bool operator==(const RhythmChoice& other) const { return tone == other.tone && exact == other.exact; }
    };
    using RhythmChoices = std::array<RhythmChoice, 64>;
    // Choices other than the built-in ones as text, for settings: "timbre:tone:shift:exact;..." and
    // "timbre:tone:exact;..." (tone as in the structs); parsing sets the rest to the built-in ones.
    static std::string presetChoicesText(const PresetChoices& choices);
    static void parsePresetChoices(const std::string& text, PresetChoices& choices);
    static std::string rhythmChoicesText(const RhythmChoices& choices);
    static void parseRhythmChoices(const std::string& text, RhythmChoices& choices);

    Mt32Translator();

    void setTarget(Target target);
    Target target() const { return target_; }
    // D20: the unit's MIDI channels (0-15, 16 = off) of parts 1-8 and rhythm, as set on its panel.
    void setUnitChannels(const uint8_t* channels);
    // Whether the MT-32's memories (patch memory -> timbre memory, timbre memory -> tones i11-i88) are written to the
    // unit, whose program changes then select them (the default). Off, the translator keeps them: a program change or
    // timbre selection sends the part's timbre temp and tone instead, and the unit's memories are left alone (unless
    // the tone cache uses them). A D-10/D-20 takes memory data only while its Memory Protect is off, which turns back on
    // at power-off, so for it this defaults to off.
    void setMemoryInUnit(bool enabled);
    // Real units have no master volume (MT-32 system 16H): on, it scales CC 7 on the parts' channels instead. After a
    // Reset All Controllers, which the D-series answer with full volume, the volume is sent again.
    void setMasterVolumeAsVolume(bool enabled);

    // The MT-32's own presets (from its control ROM), or nullptr. With them, a part on a preset is known like a
    // written timbre, so edits to its tone translate fully; with a stand-in the first edit sends the whole MT-32
    // timbre, as the unit holds a different tone.
    void setPresets(std::shared_ptr<const Mt32Presets> presets);
    const std::shared_ptr<const Mt32Presets>& presets() const { return presets_; }
    void setPresetMode(PresetMode mode);
    // The mode in effect: StandIns without presets.
    PresetMode presetMode() const;
    // d110emu: the target's preset tones hold the presets as they play, a11-b88 = A1-B64 and r01-r63 = R1-R63, so
    // they are selected by number: the MT-32's own where presetIsExact() or rhythmIsExact(), else a copy of the
    // stand-in (presetChoice().tone, rhythmStandIn()), whose key shift the translator adds. Needs setPresets().
    void setPresetBanks(bool enabled);
    bool presetBanks() const { return presetBanks_ && presets_ != nullptr; }
    // The built-in choice for an MT-32 preset (0-127), and the one in use (settable, e.g. by ear).
    static PresetChoice defaultPresetChoice(int mt32Timbre);
    void setPresetChoice(int mt32Timbre, const PresetChoice& choice);
    void resetPresetChoices();
    const PresetChoice& presetChoice(int mt32Timbre) const;
    // The same for rhythm timbres 0-63 (R1-R64).
    void setRhythmChoice(int mt32RhythmTimbre, const RhythmChoice& choice);
    void resetRhythmChoices();
    const RhythmChoice& rhythmChoice(int mt32RhythmTimbre) const;
    // Whether a preset (0-127) or rhythm timbre (0-63) plays the MT-32's own timbre (mode and choice, with presets).
    bool presetIsExact(int mt32Timbre) const;
    bool rhythmIsExact(int mt32RhythmTimbre) const;
    // The rhythm tone a rhythm timbre's stand-in is (0-62 = r01-r63, kRhythmOff = none): chosen, or built in.
    int rhythmStandIn(int mt32RhythmTimbre) const;
    // The MT-32 plays all its toms with one sample, the D-series' Tom Tom-2 (their TomTom2 set, r31-r33, used by
    // default). Roomy toms use Tom Tom-1 instead (the TomTom1 set, r28-r30, and that sample in the MT-32's timbres),
    // which suits some music better.
    void setRoomyToms(bool enabled);
    bool roomyToms() const { return roomyToms_; }

    // The tone cache, for real units while the translator keeps the memories: the tones it sends (MT-32 timbres, and
    // the MT-32's own presets) are also stored in the unit's internal tone memory, slots first-last (0-63 = i11-i88),
    // with write requests. Selecting one again then needs only a short timbre temp write instead of the 246-byte tone,
    // and rhythm keys that play MT-32 timbres get them from there. It overwrites those slots; the unit's memory
    // protect must be off. first > last turns it off.
    void setToneCache(int first, int last);
    // Forgets what the slots hold (sends nothing); they fill again as tones are used.
    void clearToneCache();
    // What each slot holds (the unit-side tone, empty if nothing known), to keep between sessions.
    std::vector<std::vector<uint8_t>> toneCacheContents() const;
    void restoreToneCache(const std::vector<std::vector<uint8_t>>& contents);
    // Stores the MT-32 side's memory timbres now, at a quiet moment, using part 8's tone (restored afterwards).
    void preloadToneCache(uint8_t deviceId, std::vector<uint8_t>& out);
    struct CacheStats {
        int slots = 0;
        int used = 0;
        uint64_t hits = 0;
        uint64_t misses = 0;
        bool failed = false;  // The unit refused a write request: the cache is off
    };
    CacheStats toneCacheStats() const;
    // The unit's answer to a write request (40 10 00: 0 done, 1 card not ready, 2 write protected, 3 wrong mode).
    // Anything but 0 turns the cache off and appends what gives the parts and rhythm keys that used it their tones
    // again. Returns false then.
    bool writeRequestResult(uint8_t result, uint8_t deviceId, std::vector<uint8_t>& out);

    // Forgets what the MT-32 side has written and assumes its power-on state.
    void reset();
    // Appends the SysEx (complete Roland DT1 messages for `deviceId`) that sets up the MT-32's power-on state: system
    // area (channels 2-10, reserves, reverb), patch memory A1-B64 and rhythm setup, the eight parts' levels and pans;
    // then to `shortAfter` the program changes that give the parts their power-on patches (where the unit holds the
    // patch memory) and volumes (master volume as CC 7). Also resets the translator.
    void powerOn(uint8_t deviceId, std::vector<uint8_t>& out, std::vector<uint32_t>& shortAfter);
    // Appends what brings the target in line after a change of presets or mode: the patch memory, rhythm setup and
    // patch temps (which reload the parts' tones) as translated now.
    void resend(uint8_t deviceId, std::vector<uint8_t>& out);

    // Appends what a short message becomes: short messages to `shortOut` (none, or one per unit channel), then SysEx
    // for `deviceId` to `sysexAfter` (a program change's timbre and tone when the translator keeps the memories or
    // the preset is the MT-32's own).
    void translateShort(uint32_t message, uint8_t deviceId, std::vector<uint32_t>& shortOut, std::vector<uint8_t>& sysexAfter);
    // Appends the translated message(s) (complete F0 ... F7 SysEx messages) for `deviceId`, the unit's device ID,
    // and to `shortAfter` any short messages that follow them (an MT-32 reset selects the power-on patches; master
    // volume as CC 7).
    void translateSysex(const uint8_t* data, size_t length, uint8_t deviceId, std::vector<uint8_t>& out,
                        std::vector<uint32_t>& shortAfter);

    // The D-110 side changed the part channels (0-15, 16 = off; parts 1-8 and rhythm), e.g. from the panel.
    void setPartChannels(const uint8_t* channels);
    // MIDI channel (0-15, 16 = off) of a part (0-7, 8 = rhythm) on the MT-32 side, and on the unit.
    uint8_t mt32Channel(int part) const;
    uint8_t unitChannel(int part) const;

    // MT-32 wave 0-127, or 128-255 for the CM-32L's second bank. `drumKit`: in one of the MT-32's own rhythm timbres
    // (R1-R64), where a few waves keep what the D-series' drum kit plays (the tuned El. Snare # stays on Snare Drum-2*,
    // which tones play as Snare Drum-1*, the user's choice).
    static const WaveMapping& waveMapping(int mt32Wave, bool drumKit = false);
    // A PCM partial's D-series wave, coarse pitch (0-96) and fine pitch (0-100), from the MT-32 partial's.
    static void translatePcm(int mt32Wave, uint8_t coarse, uint8_t fine, uint8_t& wave, uint8_t& outCoarse, uint8_t& outFine,
                             bool roomyToms = false, bool drumKit = false);
    // A whole MT-32 timbre (246 bytes, as in its timbre memory) translated for the D-series: its PCM waves and their
    // pitch (other bytes are the same). `drumKit`: one of the MT-32's own rhythm timbres (see waveMapping()).
    void convertTone(const uint8_t* mt32Timbre, uint8_t* d110Tone, bool drumKit = false) const;
    // The built-in stand-in of an MT-32 preset 0-127 as group * 64 + number, and its key shift.
    static int presetTone(int mt32Timbre);
    static int presetKeyShift(int mt32Timbre);
    // MT-32 rhythm timbre 0-63 (R1-R64) -> D-series rhythm tone 0-63 (r01-r64; kRhythmOff = none): built in (the
    // MT-32's toms), and with the tom setting.
    static int rhythmTone(int mt32RhythmTimbre, bool roomyToms = false);
    int rhythmToneFor(int mt32RhythmTimbre) const;

    // True if SysEx data writes areas an MT-32 does not have, which marks a D-series file that needs no translation.
    static bool isDSeriesData(const uint8_t* data, size_t length);

private:
    static constexpr size_t kToneSize = 246;
    struct Tone {
        std::array<uint8_t, kToneSize> data{};
        std::bitset<kToneSize> written;  // Bytes the MT-32 side has written (or loaded from a written memory tone)
        bool drumKit = false;            // One of the MT-32's rhythm timbres, selected by a part (see waveMapping())
        bool known() const { return written.all(); }
    };

    // A contiguous translated write being assembled: address (packed 7-bit) -> byte, -1 = not written.
    struct Output {
        uint32_t start = 0;
        std::vector<int16_t> bytes;
        void set(uint32_t address, uint8_t value);
    };

    struct CacheSlot {
        std::array<uint8_t, kToneSize> tone{};  // The unit-side tone it holds
        bool valid = false;
        uint64_t lastUse = 0;
    };

    void translateWrite(uint32_t address, const uint8_t* data, size_t count, uint8_t deviceId, std::vector<uint8_t>& out,
                        std::vector<uint32_t>& shortAfter);
    void writeByte(uint32_t address, uint8_t value, Output& out);
    // Timbre group, number and key shift of a patch; `part` (0-7) for a part's patch temp, -1 for patch memory.
    void translatePatch(const uint8_t* patch, uint8_t* out, int part) const;
    uint8_t translateRhythmTimbre(uint8_t value, int key) const;  // Rhythm setup timbre (M1-M64, R1-R64) of a key
    void translateTone(const Tone& tone, uint32_t base, uint32_t offset, Output& out) const;
    void translatePartial(const Tone& tone, uint32_t base, int partial, Output& out) const;
    void unitTone(const Tone& tone, uint8_t* out) const;  // A known tone as the unit gets it, all 246 bytes
    void selectTimbre(int part);  // A part's patch temp selected a timbre: load its tone as the MT-32 does
    void programChange(int channel, int program, uint8_t deviceId, std::vector<uint8_t>& sysexOut);
    void sendTones(uint8_t deviceId, std::vector<uint8_t>& out);  // Tones of parts that selected them, cache fills
    void restorePart(int part, uint8_t deviceId, std::vector<uint8_t>& out);  // After its tone temp was borrowed
    void unitChannelsFor(uint8_t channel, std::vector<uint8_t>& out) const;  // Where messages on an MT-32 channel go
    uint8_t scaledVolume(uint8_t channel) const;                            // CC 7 with the master volume
    void appendVolumes(std::vector<uint32_t>& out) const;                   // CC 7 on every part's channel
    static void emit(const Output& out, uint8_t deviceId, std::vector<uint8_t>& message);

    // Tone cache.
    bool cacheActive() const { return !cache_.empty() && !memoryInUnit_ && !cacheFailed_; }
    int cacheFind(const uint8_t* tone) const;  // Slot index holding the tone, or -1
    int cacheAllocate();                       // A free slot, else the least recently used one not in use; -1 if none
    void resolveSlot(int part);                // Where a part's tone to send goes: a slot (hit, or fill), or its temp
    int resolveRhythmKey(int key, uint8_t deviceId, std::vector<uint8_t>& out, bool& borrowed);  // Slot for a key's M timbre
    void appendToneWrite(int part, int slot, uint8_t deviceId, std::vector<uint8_t>& out) const;

    std::array<uint8_t, 0x17> system_{};
    std::array<std::array<uint8_t, 16>, 9> patchTemp_{};
    std::array<std::array<uint8_t, 4>, 85> rhythmSetup_{};
    std::array<std::array<uint8_t, 8>, 128> patchMemory_{};
    std::array<Tone, 8> toneTemp_{};
    std::array<Tone, 64> toneMemory_{};
    // Whether the target's tone temp holds the part's whole translated tone; false while a stand-in plays (the
    // part's first tone edit then sends it all). toneToSend_: a tone selected but not sent yet.
    std::array<bool, 8> toneSent_{};
    std::array<bool, 8> toneToSend_{};
    std::shared_ptr<const Mt32Presets> presets_;
    PresetMode mode_ = PresetMode::StandIns;
    bool presetBanks_ = false;
    PresetChoices presetChoices_{};
    RhythmChoices rhythmChoices_{};
    bool roomyToms_ = false;
    Target target_ = Target::D110;
    std::array<uint8_t, 9> unitChannels_{};
    bool memoryInUnit_ = true;
    bool masterVolumeAsVolume_ = false;
    std::array<uint8_t, 16> volume_{};  // CC 7 per MT-32 channel as the MT-32 side sent it (127 until then)

    std::vector<CacheSlot> cache_;
    int cacheFirst_ = 0;
    bool cacheFailed_ = false;
    uint64_t cacheClock_ = 0;
    uint64_t cacheHits_ = 0;
    uint64_t cacheMisses_ = 0;
    std::array<int, 8> partSlot_{};    // The slot a part's tone comes from on the unit, -1 = none
    std::array<bool, 8> slotFill_{};   // ...which the part's tone, when sent, fills (a write request follows)
    std::array<int, 85> rhythmSlot_{}; // The slot a rhythm key's M timbre plays from, -1 = none
};
