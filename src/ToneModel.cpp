#include "ToneModel.h"

#include <algorithm>
#include <cstdio>

#include "RolandSysex.h"

namespace Tone {

namespace {

// Wave names from the owner's manuals: the D-series' (D-110 pp.103-104, D-20 pp.260-261; the three units share
// them) and the MT-32's and CM-32L's (bank 2: the CM-32L's sound effects).
const char* const kDSeriesWaveNames[256] = {
    "Bass Drum-1", "Bass Drum-2", "Bass Drum-3", "Snare Drum-1", "Snare Drum-2", "Snare Drum-3", "Snare Drum-4",
    "Tom Tom-1", "Tom Tom-2", "High-Hat", "High-Hat (Loop)", "Crash Cymbal-1", "Crash Cymbal-2 (Loop)",
    "Ride Cymbal-1", "Ride Cymbal-2 (Loop)", "Cup", "China Cymbal-1", "China Cymbal-2 (Loop)", "Rim Shot",
    "Hand Clap", "Mute High Conga", "Conga", "Bongo", "Cowbell", "Tambourine", "Agogo", "Claves", "Timbale High",
    "Timbale Low", "Cabasa", "Timpani Attack", "Timpani", "Acoustic Piano High", "Acoustic Piano Low",
    "Piano Forte Thump", "Organ Percussion", "Trumpet", "Lips", "Trombone", "Clarinet", "Flute High", "Flute Low",
    "Steamer", "Indian Flute", "Breath", "Vibraphone High", "Vibraphone Low", "Marimba", "Xylophone High",
    "Xylophone Low", "Kalimba", "Wind Bell", "Chime Bar", "Hammer", "Guiro", "Chink", "Nails", "Fretless Bass",
    "Pull Bass", "Slap Bass", "Thump Bass", "Acoustic Bass", "Electric Bass", "Gut Guitar", "Steel Guitar",
    "Dirty Guitar", "Pizzicato", "Harp", "Contrabass", "Cello", "Violin-1", "Violin-2", "Koto", "Drawbars (Loop)",
    "High Organ (Loop)", "Low Organ (Loop)", "Trumpet (Loop)", "Trombone (Loop)", "Sax-1 (Loop)", "Sax-2 (Loop)",
    "Reed (Loop)", "Slap Bass (Loop)", "Acoustic Bass (Loop)", "Electric Bass-1 (Loop)", "Electric Bass-2 (Loop)",
    "Gut Guitar (Loop)", "Steel Guitar (Loop)", "Electric Guitar (Loop)", "Clav (Loop)", "Cello (Loop)",
    "Violin (Loop)", "Electric Piano-1 (Loop)", "Electric Piano-2 (Loop)", "Harpsichord-1 (Loop)",
    "Harpsichord-2 (Loop)", "Telephone Bell (Loop)", "Female Voice-1 (Loop)", "Female Voice-2 (Loop)",
    "Male Voice-1 (Loop)", "Male Voice-2 (Loop)", "Spectrum-1 (Loop)", "Spectrum-2 (Loop)", "Spectrum-3 (Loop)",
    "Spectrum-4 (Loop)", "Spectrum-5 (Loop)", "Spectrum-6 (Loop)", "Spectrum-7 (Loop)", "Spectrum-8 (Loop)",
    "Spectrum-9 (Loop)", "Spectrum-10 (Loop)", "Noise (Loop)", "Shot-1", "Shot-2", "Shot-3", "Shot-4", "Shot-5",
    "Shot-6", "Shot-7", "Shot-8", "Shot-9", "Shot-10", "Shot-11", "Shot-12", "Shot-13", "Shot-14", "Shot-15",
    "Shot-16", "Shot-17", "Bass Drum-1*", "Bass Drum-2*", "Bass Drum-3*", "Snare Drum-1*", "Snare Drum-2*",
    "Snare Drum-3*", "Snare Drum-4*", "Tom Tom-1*", "Tom Tom-2*", "High-Hat*", "High-Hat (Loop)*", "Crash Cymbal-1*",
    "Crash Cymbal-2 (Loop)*", "Ride Cymbal-1*", "Ride Cymbal-2 (Loop)*", "Cup*", "China Cymbal-1*",
    "China Cymbal-2 (Loop)*", "Rim Shot*", "Hand Clap*", "Mute High Conga*", "Conga*", "Bongo*", "Cowbell*",
    "Tambourine*", "Agogo*", "Claves*", "Timbale High*", "Timbale Low*", "Cabasa*", "Loop-1", "Loop-2", "Loop-3",
    "Loop-4", "Loop-5", "Loop-6", "Loop-7", "Loop-8", "Loop-9", "Loop-10", "Loop-11", "Loop-12", "Loop-13", "Loop-14",
    "Loop-15", "Loop-16", "Loop-17", "Loop-18", "Loop-19", "Loop-20", "Loop-21", "Loop-22", "Loop-23", "Loop-24",
    "Loop-25", "Loop-26", "Loop-27", "Loop-28", "Loop-29", "Loop-30", "Loop-31", "Loop-32", "Loop-33", "Loop-34",
    "Loop-35", "Loop-36", "Loop-37", "Loop-38", "Loop-39", "Loop-40", "Loop-41", "Loop-42", "Loop-43", "Loop-44",
    "Loop-45", "Loop-46", "Loop-47", "Loop-48", "Loop-49", "Loop-50", "Loop-51", "Loop-52", "Loop-53", "Loop-54",
    "Loop-55", "Loop-56", "Loop-57", "Loop-58", "Loop-59", "Loop-60", "Loop-61", "Loop-62", "Loop-63", "Loop-64",
    "Jam-1 (Loop)", "Jam-2 (Loop)", "Jam-3 (Loop)", "Jam-4 (Loop)", "Jam-5 (Loop)", "Jam-6 (Loop)", "Jam-7 (Loop)",
    "Jam-8 (Loop)", "Jam-9 (Loop)", "Jam-10 (Loop)", "Jam-11 (Loop)", "Jam-12 (Loop)", "Jam-13 (Loop)",
    "Jam-14 (Loop)", "Jam-15 (Loop)", "Jam-16 (Loop)", "Jam-17 (Loop)", "Jam-18 (Loop)", "Jam-19 (Loop)",
    "Jam-20 (Loop)", "Jam-21 (Loop)", "Jam-22 (Loop)", "Jam-23 (Loop)", "Jam-24 (Loop)", "Jam-25 (Loop)",
    "Jam-26 (Loop)", "Jam-27 (Loop)", "Jam-28 (Loop)", "Jam-29 (Loop)", "Jam-30 (Loop)", "Jam-31 (Loop)",
    "Jam-32 (Loop)", "Jam-33 (Loop)", "Jam-34 (Loop)",
};

const char* const kMt32WaveNames[256] = {
    "Ac. Bass Drum", "Ac. Snare Drum", "El. Snare Drum", "Electric Tom", "Closed Hihat", "Open Hihat", "Crash Cymbal",
    "Crash Cymbal (loop)", "Ride cymbal", "Rim Shot", "Hand Clap", "Muted Conga", "Conga", "Bongo", "Cowbell",
    "Tambourine", "Agogo Bell", "Claves", "Timbale", "Cabasa", "Keypress", "Perc Organ", "Trombone", "Trumpet",
    "Breath Noise (loop)", "Clarinet", "Flute", "Pan Pipes", "Shakuhachi", "Alto Sax", "Baritone Sax", "Marimba",
    "Vibraphone", "Xylophone", "Tubular Bells", "Fingered Bass", "Slap Bass", "Picked Bass (loop)", "Acoustic Bass",
    "Nylon Guitar", "Steel Guitar", "Pizzicato", "Harp", "Harpsichord (loop)", "Bow string", "Violin", "Timpani",
    "Orchestra Hit", "Flute 2", "Organ (loop)", "Bowed Glass (loop)", "Telephone", "Bowed Glass", "Spectrum 7 (loop)",
    "Ac. Bass Drum #", "Ac. Snare Drum #", "El. Snare Drum #", "Ac. Tom #", "Closed Hihat #", "Open Hihat (loop) #",
    "Crash Cymbal #", "Crash Cymbal (loop) #", "Ride cymbal #", "Rim shot #", "Hand clap #", "Mute Conga #",
    "Conga #", "Bongo #", "Cowbell #", "Tambourine #", "Agogo #", "Claves #", "Timbale #", "Cabasa #",
    "Bass Drum (loop)", "Snare (loop)", "El. Snare (loop)", "Electric Tom (loop)", "Hihat (loop)",
    "Crash Cymbal (loop)", "Ride cymbal (loop)", "Ride cymbal 2 (loop)", "Rim (loop)", "Hand clap (loop)",
    "Muted Conga (loop)", "Conga (loop)", "Bongo (loop)", "Cowbell (loop)", "Tambourine (loop)", "Agogo (loop)",
    "Claves (loop)", "Timbale (loop)", "Cabasa (loop)", "Keypress (loop)", "Perc Organ (loop)", "Trombone (loop)",
    "Trumpet (loop)", "Clarinet (loop)", "Flute (loop)", "Pan Pipes (loop)", "Shakuhachi (loop)", "Alto Sax (loop)",
    "Baritone Sax (loop)", "Marimba (loop)", "Vibraphone (loop)", "Xylophone (loop)", "Tubular Bells (loop)",
    "Fingered Bass (loop)", "Slap Bass (loop)", "Acoustic Bass (loop)", "Nylon Guitar (loop)", "Steel Guitar (loop)",
    "Pizzicato (loop)", "Harp (loop)", "Bow string (loop)", "Violin (loop)", "Timpani (loop)", "Orchestra Hit (loop)",
    "Flute 2 (loop)", "Perc. loop 1", "Perc. loop 2", "Orch&Perc loop", "Wind&Perc loop", "Guitar & Bass loop",
    "Orchestra loop", "Perc. loop 3", "Bass & Perc. loop", "Bass & Snare loop", "Laugh #", "Applause #",
    "Windchime #", "Crash #", "Train #", "Wind #", "Bird #", "Stream #", "Door Creak #", "Scream #", "Punch #",
    "Footsteps #", "Door Slam #", "Car Start #", "Aircraft #", "Gun Shot #", "Horse #", "Thunder #", "Bubble #",
    "Heartbeat #", "Engine #", "Tyre Screech #", "Siren #", "Helicopter #", "Dog Bark #", "Car Pass #",
    "Male Voice #", "Machine Gun #", "Starship #", "Laugh (Loop) #", "Applause (Loop) #", "Windchime (Loop) #",
    "Crash (Loop) #", "Train (Loop) #", "Wind (Loop) #", "Bird (Loop) #", "Stream (Loop) #", "Door Creak (Loop) #",
    "Scream (Loop) #", "Punch (Loop) #", "Footsteps (Loop) #", "Door Slam (Loop) #", "Car Start (Loop) #",
    "Aircraft (Loop) #", "Gun Shot (Loop) #", "Horse (Loop) #", "Thunder (Loop) #", "Bubble (Loop) #",
    "Heartbeat (Loop) #", "Engine (Loop) #", "Tyre Screech (Loop) #", "Siren (Loop) #", "Helicopter (Loop) #",
    "Dog Bark (Loop) #", "Car Pass (Loop) #", "Male Voice (Loop) #", "Machine Gun (Loop) #", "Starship (Loop) #",
    "Jam-1 (Loop)", "Jam-2 (Loop)", "Jam-3 (Loop)", "Jam-4 (Loop)", "Jam-5 (Loop)", "Jam-6 (Loop)", "Jam-7 (Loop)",
    "Jam-8 (Loop)", "Jam-9 (Loop)", "Jam-10 (Loop)", "Jam-11 (Loop)", "Jam-12 (Loop)", "Jam-13 (Loop)",
    "Jam-14 (Loop)", "Jam-15 (Loop)", "Jam-16 (Loop)", "Jam-17 (Loop)", "Jam-18 (Loop)", "Jam-19 (Loop)",
    "Jam-20 (Loop)", "Jam-21 (Loop)", "Jam-22 (Loop)", "Jam-23 (Loop)", "Jam-24 (Loop)", "Jam-25 (Loop)",
    "Jam-26 (Loop)", "Jam-27 (Loop)", "Jam-28 (Loop)", "Jam-29 (Loop)", "Jam-30 (Loop)", "Jam-31 (Loop)",
    "Jam-32 (Loop)", "Jam-33 (Loop)", "Jam-34 (Loop)", "Jam-35 (Loop)", "Jam-36 (Loop)", "Jam-37 (Loop)",
    "Jam-38 (Loop)", "Jam-39 (Loop)", "Jam-40 (Loop)", "Shot-1", "Shot-2", "Shot-3", "Shot-4", "Shot-5", "Shot-6",
    "Shot-7", "Shot-8", "Shot-9", "Shot-10", "Shot-11", "Shot-12", "Shot-13", "Shot-14", "Shot-15", "Shot-16",
    "Shot-17", "Shot-18", "Shot-19", "Shot-20", "Shot-21", "Shot-22", "Shot-23", "Shot-24", "Shot-25", "Shot-26",
    "Alto Sax", "Shakuhachi", "Marimba", "Dog Bark",
};

const char* const kPitchKeyfollow[17] = {"-1",  "-1/2", "-1/4", "0",   "1/8", "1/4", "3/8", "1/2", "5/8",
                                         "3/4", "7/8",  "1",    "5/4", "3/2", "2",   "s1",  "s2"};
const char* const kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

using F = Format;
using G = Group;
// clang-format off
const std::vector<Param> kParams = {
    // offset, group, label, display, max, initial, format, synth, pcm
    {0x00, G::Wg, "Coarse", "WG Pitch Cors", 96, 36, F::Note, true, true},
    {0x01, G::Wg, "Fine", "WG Pitch Fine", 100, 50, F::Centered50, true, true},
    {0x02, G::Wg, "Key follow", "WG Pitch KF", 16, 11, F::PitchKeyfollow, true, true},
    {0x03, G::Wg, "Bender", "WG Bender SW", 1, 1, F::OnOff, true, true},
    {0x04, G::Wg, "Waveform", "WG Waveform", 3, 1, F::Waveform, true, false},
    {0x05, G::Wg, "PCM wave", "PCM Bank / PCM", 127, 0, F::Wave, false, true},
    {0x06, G::Wg, "Pulse width", "WG Puls Width", 100, 0, F::Number, true, false},
    {0x07, G::Wg, "PW velocity", "WG PW Velo", 14, 7, F::Centered7, true, false},
    {0x08, G::PitchEnv, "Depth", "P-ENV Depth", 10, 0, F::Number, true, true},
    {0x09, G::PitchEnv, "Velocity", "P-ENV Velo", 3, 0, F::Number, true, true},
    {0x0A, G::PitchEnv, "Time key follow", "P-ENV Time KF", 4, 0, F::Number, true, true},
    {0x0B, G::PitchEnv, "Time 1", "P-ENV T1", 100, 0, F::Number, true, true},
    {0x0C, G::PitchEnv, "Time 2", "P-ENV T2", 100, 0, F::Number, true, true},
    {0x0D, G::PitchEnv, "Time 3", "P-ENV T3", 100, 0, F::Number, true, true},
    {0x0E, G::PitchEnv, "Time 4", "P-ENV T4", 100, 0, F::Number, true, true},
    {0x0F, G::PitchEnv, "Level 0", "P-ENV L0", 100, 50, F::Centered50, true, true},
    {0x10, G::PitchEnv, "Level 1", "P-ENV L1", 100, 50, F::Centered50, true, true},
    {0x11, G::PitchEnv, "Level 2", "P-ENV L2", 100, 50, F::Centered50, true, true},
    {0x12, G::PitchEnv, "Sustain level", "P-ENV Sus L", 100, 50, F::Centered50, true, true},
    {0x13, G::PitchEnv, "End level", "P-ENV End L", 100, 50, F::Centered50, true, true},
    {0x14, G::Lfo, "Rate", "P-LFO Rate", 100, 50, F::Number, true, true},
    {0x15, G::Lfo, "Depth", "P-LFO Depth", 100, 0, F::Number, true, true},
    {0x16, G::Lfo, "Modulation", "P-LFO Mod", 100, 25, F::Number, true, true},
    {0x17, G::Tvf, "Cutoff", "TVF Freq", 100, 100, F::Number, true, false},
    {0x18, G::Tvf, "Resonance", "TVF Reso", 30, 0, F::Number, true, false},
    {0x19, G::Tvf, "Key follow", "TVF Freq KF", 14, 11, F::TvfKeyfollow, true, false},
    {0x1A, G::Tvf, "Bias point", "TVF Bias P", 127, 64, F::BiasPoint, true, false},
    {0x1B, G::Tvf, "Bias level", "TVF Bias Lvl", 14, 7, F::Centered7, true, false},
    {0x1C, G::TvfEnv, "Depth", "TVF-ENV Dept", 100, 0, F::Number, true, false},
    {0x1D, G::TvfEnv, "Velocity", "TVF-ENV Velo", 100, 0, F::Number, true, false},
    {0x1E, G::TvfEnv, "Depth key follow", "TVF-ENV DKF", 4, 0, F::Number, true, false},
    {0x1F, G::TvfEnv, "Time key follow", "TVF-ENV TKF", 4, 0, F::Number, true, false},
    {0x20, G::TvfEnv, "Time 1", "TVF-ENV T1", 100, 0, F::Number, true, false},
    {0x21, G::TvfEnv, "Time 2", "TVF-ENV T2", 100, 0, F::Number, true, false},
    {0x22, G::TvfEnv, "Time 3", "TVF-ENV T3", 100, 0, F::Number, true, false},
    {0x23, G::TvfEnv, "Time 4", "TVF-ENV T4", 100, 0, F::Number, true, false},
    {0x24, G::TvfEnv, "Time 5", "TVF-ENV T5", 100, 0, F::Number, true, false},
    {0x25, G::TvfEnv, "Level 1", "TVF-ENV L1", 100, 100, F::Number, true, false},
    {0x26, G::TvfEnv, "Level 2", "TVF-ENV L2", 100, 100, F::Number, true, false},
    {0x27, G::TvfEnv, "Level 3", "TVF-ENV L3", 100, 100, F::Number, true, false},
    {0x28, G::TvfEnv, "Sustain level", "TVF-ENV Sus L", 100, 100, F::Number, true, false},
    {0x29, G::Tva, "Level", "TVA Level", 100, 100, F::Number, true, true},
    {0x2A, G::Tva, "Velocity", "TVA Velocity", 100, 50, F::Centered50, true, true},
    {0x2B, G::Tva, "Bias point 1", "TVA Bias P1", 127, 64, F::BiasPoint, true, true},
    {0x2C, G::Tva, "Bias level 1", "TVA Bias L1", 12, 12, F::Minus12, true, true},
    {0x2D, G::Tva, "Bias point 2", "TVA Bias P2", 127, 64, F::BiasPoint, true, true},
    {0x2E, G::Tva, "Bias level 2", "TVA Bias L2", 12, 12, F::Minus12, true, true},
    {0x2F, G::TvaEnv, "Time key follow", "TVA-ENV TKF", 4, 0, F::Number, true, true},
    {0x30, G::TvaEnv, "T1 velocity", "TVA-ENV T1VF", 4, 0, F::Number, true, true},
    {0x31, G::TvaEnv, "Time 1", "TVA-ENV T1", 100, 0, F::Number, true, true},
    {0x32, G::TvaEnv, "Time 2", "TVA-ENV T2", 100, 0, F::Number, true, true},
    {0x33, G::TvaEnv, "Time 3", "TVA-ENV T3", 100, 0, F::Number, true, true},
    {0x34, G::TvaEnv, "Time 4", "TVA-ENV T4", 100, 0, F::Number, true, true},
    {0x35, G::TvaEnv, "Time 5", "TVA-ENV T5", 100, 30, F::Number, true, true},
    {0x36, G::TvaEnv, "Level 1", "TVA-ENV L1", 100, 100, F::Number, true, true},
    {0x37, G::TvaEnv, "Level 2", "TVA-ENV L2", 100, 100, F::Number, true, true},
    {0x38, G::TvaEnv, "Level 3", "TVA-ENV L3", 100, 100, F::Number, true, true},
    {0x39, G::TvaEnv, "Sustain level", "TVA-ENV Sus L", 100, 100, F::Number, true, true},
};
// clang-format on

// Structures 1-13 as the firmware pairs partials (munt's PartialStruct and PartialMixStruct): the first partial of a
// pair is PCM in 3, 4, 6, 7, 9, 11 and 13, the second in 5, 6, 7, 9, 12 and 13.
const Structure kStructures[13] = {
    {false, false, Mix::Mix},  {false, false, Mix::RingPlusFirst}, {true, false, Mix::Mix},         {true, false, Mix::RingPlusFirst},
    {false, true, Mix::RingPlusFirst}, {true, true, Mix::Mix},     {true, true, Mix::RingPlusFirst}, {false, false, Mix::Stereo},
    {true, true, Mix::Stereo}, {false, false, Mix::Ring},          {true, false, Mix::Ring},        {false, true, Mix::Ring},
    {true, true, Mix::Ring},
};

std::string signedText(int value) {
    return value > 0 ? "+" + std::to_string(value) : std::to_string(value);
}

}  // namespace

const char* modelName(Model model) {
    switch (model) {
    case Model::D110: return "D-110";
    case Model::D20: return "D-10 / D-20";
    case Model::MT32: return "MT-32";
    }
    return "";
}

const char* groupName(Group group) {
    switch (group) {
    case Group::Wg: return "WG (pitch and waveform)";
    case Group::PitchEnv: return "Pitch envelope";
    case Group::Lfo: return "Pitch LFO";
    case Group::Tvf: return "TVF (filter)";
    case Group::TvfEnv: return "TVF envelope";
    case Group::Tva: return "TVA (level)";
    case Group::TvaEnv: return "TVA envelope";
    }
    return "";
}

const std::vector<Param>& partialParams() {
    return kParams;
}

const Param* findParam(int partialOffset) {
    for (const Param& param : kParams) {
        if (param.offset == partialOffset) return &param;
    }
    return nullptr;
}

std::string paramLabel(Model model, const Param& param) {
    (void)model;
    if (param.offset == Partial::TvfEnvTime1 + 4 || param.offset == Partial::TvaEnvTime1 + 4 || param.offset == Partial::PenvTime1 + 3) {
        return std::string(param.label) + " (release)";
    }
    return param.label;
}

bool hiddenOnPanel(Model model, int partialOffset) {
    if (model != Model::D20) return false;
    return partialOffset == Partial::PenvSustain || partialOffset == Partial::TvfEnvTime1 + 3 || partialOffset == Partial::TvfEnvLevel1 + 2 ||
           partialOffset == Partial::TvaEnvTime1 + 3 || partialOffset == Partial::TvaEnvLevel1 + 2;
}

std::string noteName(int midiNote) {
    midiNote = std::clamp(midiNote, 0, 127);
    return std::string(kNoteNames[midiNote % 12]) + std::to_string(midiNote / 12 - 1);
}

const char* waveName(Model model, int bank, int number) {
    const int index = (bank & 1) * 128 + (number & 127);
    return model == Model::MT32 ? kMt32WaveNames[index] : kDSeriesWaveNames[index];
}

std::string formatValue(Model model, const Param& param, int value, int bank) {
    switch (param.format) {
    case Format::Number: return std::to_string(value);
    case Format::Centered50: return signedText(value - 50);
    case Format::Centered7: return signedText(value - 7);
    case Format::Minus12: return std::to_string(value - 12);
    case Format::OnOff: return value ? "On" : "Off";
    case Format::Note: return noteName(24 + std::clamp(value, 0, 96));  // Coarse 0 = C1
    case Format::PitchKeyfollow:
    case Format::TvfKeyfollow: return kPitchKeyfollow[std::clamp(value, 0, param.format == Format::TvfKeyfollow ? 14 : 16)];
    case Format::BiasPoint: return std::string(value < 64 ? "<" : ">") + noteName(33 + (value & 63));  // A1..C7
    case Format::Waveform: return (value & 1) ? "SAW" : "SQU";
    case Format::Wave: {
        char text[16];
        std::snprintf(text, sizeof(text), "%d-%03d ", (bank & 1) + 1, (value & 127) + 1);
        return text + std::string(waveName(model, bank, value));
    }
    }
    return std::to_string(value);
}

uint8_t maxValue(int offset) {
    if (offset < 0 || offset >= kSize) return 0;
    if (offset < kCommonSize) {
        static const uint8_t common[kCommonSize] = {127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 12, 12, 15, 1};
        return common[offset];
    }
    const Param* param = findParam((offset - kCommonSize) % kPartialSize);
    return param != nullptr ? param->max : 0;
}

void clamp(Data& tone) {
    for (int i = 0; i < kSize; i++) {
        if (i < kNameLength) {
            if (tone[size_t(i)] < 32 || tone[size_t(i)] > 127) tone[size_t(i)] = ' ';
        } else {
            tone[size_t(i)] = std::min(tone[size_t(i)], maxValue(i));
        }
    }
}

const Structure& structure(int value) {
    return kStructures[std::clamp(value, 0, 12)];
}

bool isPcm(const Data& tone, int partial) {
    const Structure& s = structure(tone[size_t(partial < 2 ? Common::Structure12 : Common::Structure34)]);
    return (partial & 1) == 0 ? s.firstPcm : s.secondPcm;
}

std::string structureText(int value, int first) {
    const Structure& s = structure(value);
    const std::string a = std::string(s.firstPcm ? "P" : "S") + std::to_string(first);
    const std::string b = std::string(s.secondPcm ? "P" : "S") + std::to_string(first + 1);
    switch (s.mix) {
    case Mix::Mix: return a + " + " + b;
    case Mix::RingPlusFirst: return a + " + R(" + a + "," + b + ")";
    case Mix::Ring: return "R(" + a + "," + b + ")";
    case Mix::Stereo: return a + " L, " + b + " R";
    }
    return "";
}

std::string name(const Data& tone) {
    std::string text(reinterpret_cast<const char*>(tone.data()), kNameLength);
    for (char& c : text) {
        if (uint8_t(c) < 32 || uint8_t(c) > 126) c = ' ';
    }
    text.erase(text.find_last_not_of(' ') + 1);
    return text;
}

void setName(Data& tone, const std::string& name) {
    for (int i = 0; i < kNameLength; i++) {
        const char c = i < int(name.size()) ? name[size_t(i)] : ' ';
        tone[size_t(i)] = uint8_t(c) < 32 || uint8_t(c) > 126 ? uint8_t(' ') : uint8_t(c);
    }
}

bool partialOn(const Data& tone, int partial) {
    return (tone[Common::PartialMute] >> partial) & 1;
}

void initPartial(Data& tone, int partial) {
    for (const Param& param : kParams) tone[size_t(partialBase(partial) + param.offset)] = param.initial;
}

Data initialTone() {
    Data tone{};
    setName(tone, "New Tone");
    tone[Common::Structure12] = 0;
    tone[Common::Structure34] = 0;
    tone[Common::PartialMute] = 0x01;
    tone[Common::EnvMode] = 0;
    for (int partial = 0; partial < kPartials; partial++) initPartial(tone, partial);
    return tone;
}

std::vector<EnvelopePoint> envelopePoints(Model model, Envelope envelope) {
    (void)model;  // The same engine in every unit
    std::vector<EnvelopePoint> points;
    if (envelope == Envelope::Pitch) {
        points.push_back({-1, Partial::PenvLevel0, 0, false});
        points.push_back({Partial::PenvTime1, Partial::PenvLevel0 + 1, 0, false});
        points.push_back({Partial::PenvTime1 + 1, Partial::PenvLevel0 + 2, 0, false});
        points.push_back({Partial::PenvTime1 + 2, Partial::PenvSustain, 0, false});
        points.push_back({Partial::PenvTime1 + 3, Partial::PenvEnd, 0, true});
        return points;
    }
    const int time = envelope == Envelope::Tvf ? Partial::TvfEnvTime1 : Partial::TvaEnvTime1;
    const int level = envelope == Envelope::Tvf ? Partial::TvfEnvLevel1 : Partial::TvaEnvLevel1;
    const int sustain = envelope == Envelope::Tvf ? Partial::TvfEnvSustain : Partial::TvaEnvSustain;
    points.push_back({-1, -1, 0, false});
    points.push_back({time, level, 0, false});
    points.push_back({time + 1, level + 1, 0, false});
    points.push_back({time + 2, level + 2, 0, false});
    points.push_back({time + 3, sustain, 0, false});
    points.push_back({time + 4, -1, 0, true});
    return points;
}

uint32_t toneTempAddress(int part) {
    if (part >= 9) return RolandSysex::pack(0x140000) + uint32_t(part - 9) * kSize;
    return RolandSysex::pack(0x040000) + uint32_t(std::clamp(part, 0, 7)) * kSize;
}

uint32_t toneMemoryAddress(int slot) {
    return RolandSysex::pack(0x080000) + uint32_t(slot & 63) * 256;
}

uint32_t cardToneAddress(int slot) {
    return RolandSysex::pack(0x180000) + uint32_t(slot & 63) * 256;
}

std::string slotName(int slot) {
    return std::string("i") + char('1' + (slot & 63) / 8) + char('1' + (slot & 7));
}

std::vector<FoundTone> findTones(const uint8_t* sysex, size_t length) {
    struct Region {
        FoundTone::Area area;
        uint32_t base;
        int first;   // Index of the first entry (parts 9-15 start at 9)
        int count;
        uint32_t stride;
        std::vector<uint8_t> bytes;
        std::vector<bool> written;
    };
    std::vector<Region> regions = {
        {FoundTone::Area::Temporary, toneTempAddress(0), 0, 8, uint32_t(kSize), {}, {}},
        {FoundTone::Area::Temporary, toneTempAddress(9), 9, 7, uint32_t(kSize), {}, {}},
        {FoundTone::Area::Memory, toneMemoryAddress(0), 0, 64, 256, {}, {}},
        {FoundTone::Area::Card, cardToneAddress(0), 0, 64, 256, {}, {}},
    };
    for (Region& region : regions) {
        region.bytes.assign(size_t(region.count) * region.stride, 0);
        region.written.assign(region.bytes.size(), false);
    }
    RolandSysex::forEachMessage(sysex, length, [&](const uint8_t* message, size_t size) {
        RolandSysex::DataMessage data;
        if (!RolandSysex::parseDataSet(message, size, data)) return;
        for (Region& region : regions) {
            const uint32_t end = region.base + uint32_t(region.bytes.size());
            if (data.address + data.length <= region.base || data.address >= end) continue;
            for (size_t i = 0; i < data.length; i++) {
                const uint32_t address = data.address + uint32_t(i);
                if (address < region.base || address >= end) continue;
                region.bytes[address - region.base] = data.data[i];
                region.written[address - region.base] = true;
            }
        }
    });
    std::vector<FoundTone> tones;
    for (const Region& region : regions) {
        for (int entry = 0; entry < region.count; entry++) {
            const size_t start = size_t(entry) * region.stride;
            if (!std::all_of(region.written.begin() + long(start), region.written.begin() + long(start + kSize), [](bool b) { return b; })) continue;
            FoundTone tone;
            tone.area = region.area;
            tone.index = region.first + entry;
            std::copy(region.bytes.begin() + long(start), region.bytes.begin() + long(start + kSize), tone.data.begin());
            switch (region.area) {
            case FoundTone::Area::Temporary: tone.where = "Part " + std::to_string(tone.index < 8 ? tone.index + 1 : tone.index); break;
            case FoundTone::Area::Memory: tone.where = slotName(entry); break;
            case FoundTone::Area::Card: tone.where = "c" + slotName(entry).substr(1); break;
            }
            tones.push_back(tone);
        }
    }
    return tones;
}

std::vector<uint8_t> toneMemoryDump(const std::vector<std::pair<int, Data>>& slots, uint8_t device) {
    std::vector<uint8_t> out;
    for (const auto& slot : slots) RolandSysex::appendDataSet(out, device, toneMemoryAddress(slot.first), slot.second.data(), kSize);
    return out;
}

}  // namespace Tone
