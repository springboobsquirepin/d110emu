#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// The LA tone: the D-110's, D-10's and D-20's tone, the MT-32's timbre. 246 bytes, as in the tone temporary area
// (04 00 00 + F6H per part) and tone memory (08 00 00 + 100H per tone): a common block of 14 bytes and four partials
// of 58. Names, ranges and display follow the owner's manuals (D-110 pp.98-99 and 117-118, D-20 pp.259 and 266); the
// ranges are the D-110 control ROM's own table of maximum values, which equals the MT-32's.
namespace Tone {

constexpr int kSize = 246;
constexpr int kCommonSize = 14;
constexpr int kPartialSize = 58;
constexpr int kPartials = 4;
constexpr int kNameLength = 10;
using Data = std::array<uint8_t, kSize>;

constexpr int partialBase(int partial) {
    return kCommonSize + kPartialSize * partial;
}

// Offsets in the common block.
namespace Common {
constexpr int Name = 0x00, Structure12 = 0x0A, Structure34 = 0x0B, PartialMute = 0x0C, EnvMode = 0x0D;
}

// Offsets in a partial (add partialBase()).
namespace Partial {
constexpr int PitchCoarse = 0x00, PitchFine = 0x01, PitchKeyfollow = 0x02, BenderSwitch = 0x03, Waveform = 0x04, PcmWave = 0x05,
              PulseWidth = 0x06, PwVelocity = 0x07, PenvDepth = 0x08, PenvVelocity = 0x09, PenvTimeKeyfollow = 0x0A,
              PenvTime1 = 0x0B, PenvLevel0 = 0x0F, PenvSustain = 0x12, PenvEnd = 0x13, LfoRate = 0x14, LfoDepth = 0x15,
              LfoModulation = 0x16, TvfCutoff = 0x17, TvfResonance = 0x18, TvfKeyfollow = 0x19, TvfBiasPoint = 0x1A,
              TvfBiasLevel = 0x1B, TvfEnvDepth = 0x1C, TvfEnvVelocity = 0x1D, TvfEnvDepthKeyfollow = 0x1E,
              TvfEnvTimeKeyfollow = 0x1F, TvfEnvTime1 = 0x20, TvfEnvLevel1 = 0x25, TvfEnvSustain = 0x28, TvaLevel = 0x29,
              TvaVelocity = 0x2A, TvaBiasPoint1 = 0x2B, TvaBiasLevel1 = 0x2C, TvaBiasPoint2 = 0x2D, TvaBiasLevel2 = 0x2E,
              TvaEnvTimeKeyfollow = 0x2F, TvaEnvTimeVelocity = 0x30, TvaEnvTime1 = 0x31, TvaEnvLevel1 = 0x36,
              TvaEnvSustain = 0x39;
}

// Which unit the tone is for. The D-110, D-10 and D-20 share one sound engine and all of the tone's bytes: the D-20's
// panel leaves out the TVF and TVA envelopes' fourth time and third level and the pitch envelope's sustain level, and its
// manual (p.266) calls them "dummy (for MT-32)", but the unit plays them (the user heard Level 3 on a real D-20). The
// MT-32's waves differ from the D-series' (whose 256 waves the three D units share); the units' setups differ too.
enum class Model { D110, D20, MT32 };
const char* modelName(Model model);

enum class Format : uint8_t {
    Number,          // 0..max as is
    Centered50,      // 0-100 shown as -50..+50
    Centered7,       // 0-14 shown as -7..+7
    Minus12,         // 0-12 shown as -12..0
    OnOff,
    Note,            // Pitch coarse 0-96: C1..C9
    PitchKeyfollow,  // 0-16: -1, -1/2, -1/4, 0, 1/8 ... 2, s1, s2
    TvfKeyfollow,    // 0-14: -1 ... 2
    BiasPoint,       // 0-127: <A1..<C7, >A1..>C7
    Waveform,        // Bit 0 of the waveform/PCM bank byte: SQU, SAW
    Wave,            // PCM wave number 0-127, in the bank that bit 1 of the waveform/PCM bank byte chooses
};

enum class Group : uint8_t { Wg, PitchEnv, Lfo, Tvf, TvfEnv, Tva, TvaEnv };
const char* groupName(Group group);

// One partial parameter (one byte).
struct Param {
    uint8_t offset;       // In the partial
    Group group;
    const char* label;    // Short label for the editor
    const char* display;  // The D-110's display (manual p.99)
    uint8_t max;
    uint8_t initial;      // Value in an initialised tone
    Format format;
    bool synth;           // Takes effect on a synthesizer partial
    bool pcm;             // Takes effect on a PCM partial
};

// The 58 partial parameters in the order the manuals list them.
const std::vector<Param>& partialParams();
const Param* findParam(int partialOffset);
// The editor's label: "Time 5 (release)" and so on.
std::string paramLabel(Model model, const Param& param);
// A parameter the model's own panel does not show (the D-10/D-20's), although the unit plays it (see Model).
bool hiddenOnPanel(Model model, int partialOffset);

// Value as the unit displays it: "C4", "+12", "<A#3", "SAW", "1-043 Steamer".
std::string formatValue(Model model, const Param& param, int value, int bank = 0);
std::string noteName(int midiNote);  // 60 = "C4"

// Maximum of any of the 246 bytes (the unit clamps larger values).
uint8_t maxValue(int offset);
// A tone with every byte in range; names keep printable ASCII.
void clamp(Data& tone);

// Structures 1-13 (stored 0-12): which partial of the pair is PCM, and how the pair is mixed.
enum class Mix : uint8_t { Mix, RingPlusFirst, Ring, Stereo };
struct Structure {
    bool firstPcm;   // Partial 1 (or 3)
    bool secondPcm;  // Partial 2 (or 4)
    Mix mix;
};
const Structure& structure(int value);
bool isPcm(const Data& tone, int partial);
// "S1 + S2", "P1 + Ring (P1 x S2)", ... for partials 1-2 (first = 1) or 3-4 (first = 3).
std::string structureText(int value, int first);

// The wave name for bank 0-1 and number 0-127; "" for a synthesizer partial.
const char* waveName(Model model, int bank, int number);

std::string name(const Data& tone);
void setName(Data& tone, const std::string& name);
bool partialOn(const Data& tone, int partial);  // The partial mute bit is set (1 = sounds)

// A plain starting tone: partial 1 only, a sawtooth at C4 through an open filter.
Data initialTone();
void initPartial(Data& tone, int partial);

// Envelope points as the unit's envelope runs through them, for drawing: the pitch envelope (levels 0-100 = -50..+50),
// TVF or TVA (levels 0-100). `attack` times lead up to the sustain point; the last point is the release's end.
enum class Envelope { Pitch, Tvf, Tva };
struct EnvelopePoint {
    int timeOffset;   // Partial offset of the time that leads to this point, -1 for the start
    int levelOffset;  // Partial offset of the level, -1 for a fixed level
    int fixedLevel;   // When levelOffset is -1
    bool afterKeyOff; // The release's end
};
std::vector<EnvelopePoint> envelopePoints(Model model, Envelope envelope);

// Where tones live in the address map (packed addresses, see RolandSysex.h).
uint32_t toneTempAddress(int part);   // Parts 0-7: 04 00 00 + F6H each; 9-15 (d110emu's 16-part mode): 14 00 00
uint32_t toneMemoryAddress(int slot); // i11-i88 (the MT-32's memory timbres): 08 00 00 + 100H each
uint32_t cardToneAddress(int slot);   // d110emu's memory card, c11-c88: 18 00 00 + 100H each
std::string slotName(int slot);       // 0-63 -> "i11".."i88"

// Tones in SysEx data (a file, a bulk dump, a handshake transfer's DAT packets, or what a unit sent): every tone whose
// 246 bytes are all there, in the parts' temporary areas, tone memory or d110emu's card.
struct FoundTone {
    enum class Area { Temporary, Memory, Card };
    Area area = Area::Memory;
    int index = 0;      // Part (0-15) or slot (0-63)
    std::string where;  // "Part 2", "i11", "c11"
    Data data{};
};
std::vector<FoundTone> findTones(const uint8_t* sysex, size_t length);
// DT1 messages that load tones into tone memory (i11-i88 = slots 0-63, 246 bytes each), device `device`.
std::vector<uint8_t> toneMemoryDump(const std::vector<std::pair<int, Data>>& slots, uint8_t device);

}  // namespace Tone
