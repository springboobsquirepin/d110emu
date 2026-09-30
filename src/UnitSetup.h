#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "RolandSysex.h"
#include "ToneModel.h"

// ToneEditor's view of a real unit's setup, besides the tone: what each part plays and how (the timbre temporary areas:
// tone, key shift, fine tune, bender range, assign mode, output, level, pan, key range), timbre memory A11-B88 (the
// MT-32's patch memory), the rhythm setup of keys 24-108, and the system area (master tune, reverb, partial reserves,
// MIDI channels). It keeps a copy of what the unit sent (RQ1 at the areas' base addresses) and sends every change at
// once as a DT1, like the tone editor.
//
// The D-110, the D-10/D-20 and the MT-32 lay these areas out alike (the MT-32's map). They differ in:
//  - output (byte 06 of a part, byte 03 of a rhythm key): the D-110's output assign (Mix, Mix + reverb, Multi 1-6),
//    the others' reverb switch;
//  - key ranges (bytes 0A/0B): the D-110's; dummies on the others;
//  - part channels (system 0D-15): the D-110 and MT-32 take them over SysEx; a D-20 sets them on its panel, and its
//    system area (50 bytes) has dummies there, then part levels and pans (21-31) besides the timbre temporary areas';
//  - reverb types: 9 on the D-series (Small/Medium Room, Medium/Large Hall, Plate, Delay 1/2/3, Off), 4 on the MT-32;
//  - panpots: left to right on the D-series, right to left on the MT-32; the MT-32 has a master volume (system 16).
class UnitSetup {
public:
    static constexpr int kParts = 9;             // Parts 1-8 and the rhythm part (8)
    static constexpr int kRhythmPart = 8;
    static constexpr int kPartSize = 16;         // A timbre temporary area
    static constexpr int kRhythmKeys = 85;       // Keys 24-108
    static constexpr int kFirstRhythmKey = 24;
    static constexpr int kMaxSystemSize = 0x32;  // The D-20's system area
    static constexpr int kTimbres = 128;         // Timbre memory A11-B88 (the MT-32's patch memory 1-128)
    static constexpr int kTimbreSize = 8;        // Laid out as a part's bytes 00-07

    // Offsets in a part's timbre temporary area (the MT-32's patch temporary area).
    enum PartOffset : int { ToneGroup = 0, ToneNumber = 1, KeyShift = 2, FineTune = 3, BenderRange = 4, AssignMode = 5, Output = 6,
                            Level = 8, Panpot = 9, KeyLow = 10, KeyHigh = 11 };
    // Offsets in a rhythm key's setup.
    enum RhythmOffset : int { RhythmTone = 0, RhythmLevel = 1, RhythmPanpot = 2, RhythmOutput = 3 };
    // Offsets in the system area.
    enum SystemOffset : int { MasterTune = 0, ReverbMode = 1, ReverbTime = 2, ReverbLevel = 3, Reserves = 4, Channels = 0x0D,
                              MasterVolume = 0x16 };

    class Host {
    public:
        virtual ~Host() = default;
        // DT1 to the unit. `merge`: a waiting DT1 for the same bytes may take this data instead (parameter changes; not
        // write requests).
        virtual void sendData(uint32_t packedAddress, const uint8_t* data, size_t length, bool merge) = 0;
        virtual void request(uint32_t packedAddress, uint32_t size) = 0;                      // RQ1
        virtual void sendShort(uint32_t message) = 0;
        virtual std::string memoryToneName(int slot) = 0;  // i01-i64 (the MT-32's memory timbres), "" if not read
    };

    // Packed addresses.
    static uint32_t partAddress(int part);      // Part 0-7, or 8: the rhythm part (03 01 00)
    static uint32_t rhythmKeyAddress(int key);  // Key 24-108 (03 01 10 + 4 per key)
    static constexpr uint32_t kSystemAddress = RolandSysex::pack(0x100000);
    static constexpr uint32_t kTimbreMemoryAddress = RolandSysex::pack(0x050000);
    static constexpr uint32_t kTimbreWriteAddress = RolandSysex::pack(0x400100);  // + 2 per part: timbre, internal/card
    static uint32_t timbreAddress(int timbre);  // 05 00 00 + 8 per timbre
    static int systemSize(Tone::Model model);   // 21H (D-110), 32H (D-10/D-20), 17H (MT-32)

    UnitSetup();
    void setModel(Tone::Model model);
    Tone::Model model() const { return model_; }
    // Asks the unit for its whole setup: the nine parts, timbre memory, the rhythm setup and the system area.
    void requestAll(Host& host);
    void requestPart(Host& host, int part);
    void requestTimbres(Host& host);  // Four requests of 32 timbres
    // A DT1 from the unit: the bytes that fall into the setup's areas are kept. True if any did.
    bool take(uint32_t packedAddress, const uint8_t* data, size_t length);
    void forget();

    // Values as the unit holds them, -1 if not read yet.
    int partValue(int part, int offset) const;
    int rhythmValue(int key, int offset) const;
    int systemValue(int offset) const;
    int timbreValue(int timbre, int offset) const;
    // Changes, sent at once (values clamped to the model's ranges).
    void setPartValue(Host& host, int part, int offset, int value);
    void setPartTone(Host& host, int part, int group, int number);  // Group and number together: the part loads the tone
    void setRhythmValue(Host& host, int key, int offset, int value);
    void setSystemValue(Host& host, int offset, int value);
    void setReserve(Host& host, int part, int value);  // Sent as the package of all nine parts, as the units need
    // Timbre memory: a byte of a timbre (00-06: tone group, tone number, key shift, fine tune, bender range, assign mode,
    // output), the tone (00-01 together) or a whole timbre (paste). Sent at once to timbre memory, and to the timbre
    // temporary areas of parts that play the timbre (the program changes the editor sent), so the change is heard. True
    // when the edited part's tone changed (the tone editor must read it again).
    bool setTimbreValue(Host& host, int timbre, int offset, int value, int editedPart);
    bool setTimbreTone(Host& host, int timbre, int group, int number, int editedPart);
    bool pasteTimbre(Host& host, int timbre, const uint8_t* bytes, int editedPart);  // Bytes 00-06
    // Plays a timbre on a part: a program change on its channel, then the part is read again. False without a channel.
    bool selectTimbre(Host& host, int part, int timbre);
    int partTimbre(int part) const;  // The timbre the editor last selected on the part, -1 if not known
    // The unit's timbre write: stores what the part plays (its timbre temporary area) as a timbre of memory. The D-110
    // and D-10/D-20 have it (the D-20 in multi-timbral mode); the MT-32 does not, so its patch memory is written directly.
    void writeTimbre(Host& host, int part, int timbre);
    // The parts' MIDI channels (0-15, 16 = off). Changing one writes the unit's system area where it has them (D-110,
    // MT-32); a D-20's are set on its panel, and the copy here only tells the editor where to play.
    std::array<uint8_t, kParts>& channels() { return channels_; }
    const std::array<uint8_t, kParts>& channels() const { return channels_; }
    void setChannel(Host& host, int part, int channel);
    // The channels the unit reported in its system area, if it has them and sent them since the last call. A D-10/D-20
    // has dummies there (the same for all nine parts), which are not taken: channelsMissing() tells.
    bool takeChannelsFromUnit(std::array<uint8_t, kParts>& channels);
    bool channelsMissing() const { return channelsMissing_; }

    // D-10/D-20 performance mode: the performance patch (the patch temporary area, 03 04 00) and patch memory A11-B88
    // (07 00 00), 38 bytes each. The upper tone plays in part 1's tone temporary area, the lower in part 2's. Program
    // changes on the performance channel (the keyboard's) select patches.
    static constexpr int kPatchSize = 38;
    static constexpr int kPatches = 128;
    static constexpr int kPatchNameLength = 16;
    static constexpr uint32_t kPatchTempAddress = RolandSysex::pack(0x030400);
    static constexpr uint32_t kPatchMemoryAddress = RolandSysex::pack(0x070000);
    static constexpr uint32_t kPatchWriteAddress = RolandSysex::pack(0x400300);  // Patch, internal/card (performance mode only)
    static uint32_t patchAddress(int patch);
    enum PatchOffset : int { KeyMode = 0x00, SplitPoint = 0x01, LowerToneGroup = 0x02, LowerToneNumber = 0x03, UpperToneGroup = 0x04,
                             UpperToneNumber = 0x05, LowerKeyShift = 0x06, UpperKeyShift = 0x07, LowerFineTune = 0x08,
                             UpperFineTune = 0x09, LowerBender = 0x0A, UpperBender = 0x0B, LowerAssign = 0x0C, UpperAssign = 0x0D,
                             LowerReverb = 0x0E, UpperReverb = 0x0F, PatchReverbMode = 0x10, PatchReverbTime = 0x11,
                             PatchReverbLevel = 0x12, Balance = 0x13, PatchLevel = 0x14, PatchName = 0x15 };
    void requestPatch(Host& host);      // The performance patch and patch memory
    void requestPatchTemp(Host& host);  // The performance patch only
    int patchValue(int offset) const;  // The performance patch's byte, -1 if not read
    int patchMemoryValue(int patch, int offset) const;
    std::string patchName(int patch) const;  // A patch of memory, "" if not read
    void setPatchValue(Host& host, int offset, int value);
    void setPatchTone(Host& host, bool upper, int group, int number);
    void setPatchName(Host& host, const std::string& name);
    // Selects a patch: a program change on the performance channel, then the performance patch is read again.
    bool selectPatch(Host& host, int patch);
    // The unit's patch write: stores the performance patch as a patch of memory (performance mode only).
    void writePatch(Host& host, int patch);
    int currentPatch() const { return currentPatch_; }
    uint8_t& performanceChannel() { return performanceChannel_; }
    uint8_t performanceChannel() const { return performanceChannel_; }

    // UI (Dear ImGui). The Parts tab reports a click on a part's number (edit its tone), and changes to what the edited
    // part plays (its tone must be read again).
    struct PartsEvents {
        int editPart = -1;
        bool editedToneChanged = false;
    };
    void drawParts(Host& host, int editedPart, PartsEvents& events);
    void drawTimbres(Host& host, int editedPart, PartsEvents& events);
    // The Performance tab (D-10/D-20) reports a request to edit the upper (part 1) or lower (part 2) tone, and which
    // tones changed.
    struct PatchEvents {
        int editTone = -1;  // Part 0 (upper) or 1 (lower)
        bool upperToneChanged = false;
        bool lowerToneChanged = false;
    };
    void drawPatch(Host& host, PatchEvents& events);
    void drawRhythm(Host& host, double now);
    void drawSystem(Host& host);
    // Plays a rhythm key on the rhythm part's channel (the Rhythm tab's Play), released half a second later by update().
    // False when the rhythm part has no channel.
    bool playRhythmKey(Host& host, int key, double now);
    // Note offs of the Rhythm tab's audition.
    void update(Host& host, double now);

    // Display helpers.
    static std::string masterTuneText(int value);  // "440.0 Hz"
    std::string toneLabel(Host& host, int group, int number) const;  // "a11 AcouPiano1", "M05 Brass"
    std::string rhythmToneLabel(Host& host, int value) const;        // "r05 Snare Drum", "OFF"
    std::string timbreCode(int timbre) const;                        // "A11" (D-series), "  1" (MT-32 patch)
    // How to let the unit take changes to its memory (D-110, D-10/D-20): Memory Protect off.
    const char* memoryProtectNote() const;

private:
    struct Area {
        std::vector<uint8_t> bytes;
        std::vector<bool> known;
        void reset(size_t size) {
            bytes.assign(size, 0);
            known.assign(size, false);
        }
    };
    int partMax(int offset) const;
    int patchMax(int offset) const;
    int rhythmMax(int offset) const;
    int systemMax(int offset) const;
    void sendPart(Host& host, int part, int offset, int length);
    // The tone popup ("tones", opened by the caller): true when a tone was chosen.
    bool tonePopup(Host& host, int group, int number, int& chosenGroup, int& chosenNumber);
    bool drawProgramPicker(Host& host, int part);  // True when a program change was sent

    Tone::Model model_ = Tone::Model::D110;
    Area parts_;   // 9 x 16 bytes from 03 00 00
    Area rhythm_;  // 85 x 4 bytes from 03 01 10
    Area system_;  // Up to 50 bytes from 10 00 00
    Area timbres_; // 128 x 8 bytes from 05 00 00
    std::array<int, kParts> partTimbre_;  // Timbre last selected on each part by the editor, -1 unknown
    Area patch_;   // The performance patch, 38 bytes from 03 04 00
    Area patches_; // 128 x 38 bytes from 07 00 00
    int currentPatch_ = -1;
    int patchWriteTarget_ = 0;
    uint8_t performanceChannel_ = 0;
    std::array<uint8_t, kTimbreSize> timbreClipboard_{};
    bool timbreClipboardFull_ = false;
    int writeTarget_ = 0;
    std::array<uint8_t, kParts> channels_ = {0, 1, 2, 3, 4, 5, 6, 7, 9};
    bool channelsFromUnit_ = false;
    bool channelsMissing_ = false;  // The unit's system area had no part channels (a D-10/D-20 set as another unit)
    struct PendingOff {
        double time;
        uint32_t message;
    };
    std::vector<PendingOff> noteOffs_;
    int auditionVelocity_ = 100;
};
