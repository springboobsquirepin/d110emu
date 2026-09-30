#pragma once

#include <string>

// Factory names, as the owner's manuals list them: the MT-32's preset timbres A1-B64, and the D-series' preset tones
// (a11-a88, b11-b88) and rhythm tones (r01-r64), indexed like Mt32Translator::PresetChoice::tone (the i tones,
// 128-191, are the unit's own and have no fixed name).
extern const char* const kMt32PresetNames[128];
extern const char* const kDSeriesToneNames[256];
// The MT-32's rhythm timbres R1-R30 (as its control ROM names them) and the CM-32L's sound effects R31-R63.
extern const char* const kMt32RhythmNames[64];

// A D-series preset or rhythm tone as the unit numbers it, with its name: "a11 AcouPiano1", "r05 Crash Cym" (tone as
// Mt32Translator::PresetChoice::tone: a11-b88 = 0-127, r01-r64 = 192-255).
std::string dSeriesToneLabel(int tone);
