#pragma once

#include <string>

#include "Mt32Translator.h"

// Tables of what each MT-32 preset (A1-B64) and rhythm timbre (R1-R63) plays: the D-series' closest preset or rhythm
// tone (a stand-in; presets with a key shift where the unit's is an octave away) or, in Hybrid mode, the MT-32's own.
// Used by MT32Translator's Presets tab and d110emu's MT-32 presets window.
namespace PresetChoiceTable {

struct Context {
    const Mt32Presets* presets = nullptr;  // The MT-32's own (their names, and whether they can play); nullptr = none
    Mt32Translator::PresetMode mode = Mt32Translator::PresetMode::StandIns;
    const char* unitName = "Unit's";       // What the Plays column calls the stand-ins
    bool roomyToms = false;                // Mt32Translator::setRoomyToms, which changes the toms' built-in stand-ins
};

// A D-series preset or rhythm tone, numbered as Mt32Translator::PresetChoice::tone: "a11 AcouPiano1", "r05 Crash Cym".
std::string toneLabel(int tone);

// Each draws its table in the space left and returns true when a choice changed.
bool drawPresets(Mt32Translator::PresetChoices& choices, const Context& context);
bool drawRhythm(Mt32Translator::RhythmChoices& choices, const Context& context);

}  // namespace PresetChoiceTable
