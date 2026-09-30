#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "SmfFile.h"

// D-20 rhythm patterns and rhythm track, as a D-20 sends them in a bulk dump (its MIDI implementation, 8-6 and 8-7).
// A pattern is one bar of 1/4 to 8/4 with up to 96 notes on a grid of 24 steps per quarter note; the rhythm track,
// the song of the D-20's rhythm machine, lists up to 500 bars, each a pattern or a blank bar. The patterns P-11-P-48
// are the D-20's presets, which are in its ROM and never in a dump; P-51-P-88 are programmable.
constexpr int kD20PatternCount = 64;     // P-11-P-48, then P-51-P-88
constexpr int kD20PresetPatterns = 32;
constexpr int kD20StepsPerQuarter = 24;
constexpr size_t kD20PatternSize = 0x24C;     // Nibbles per pattern in memory and SysEx
constexpr size_t kD20RhythmTrackSize = 0x1F6;
constexpr int kD20TrackBlank = 64;           // Track entries 64-71: a blank bar of 1-8 beats

struct D20Pattern {
    struct Note {
        int step;      // 0 to beats * 24 - 1
        int key;       // 24-108: the rhythm setup's keys
        int velocity;  // 1-127
    };
    int beats = 4;             // The bar is beats/4 long (1-8)
    std::vector<Note> notes;   // In step order
    bool present = false;      // The slot holds data (a present pattern may still have no notes)
};

struct D20Rhythm {
    std::array<D20Pattern, kD20PatternCount> patterns;
    std::vector<int> track;  // Per bar: 0-63 a pattern (P-11-P-88), 64-71 a blank bar of 1-8 beats
};

// Parses the rhythm memory: 64 patterns of kD20PatternSize nibbles (P-11 first) and the rhythm track.
// Erased notes (key 128) and unused slots are skipped.
D20Rhythm parseD20Rhythm(const uint8_t* patterns, const uint8_t* track);

// A pattern in that format: 294 bytes (time signature, number of notes, a dummy, 96 events of step, key and velocity,
// the end mark FFH and two dummies) as 588 nibbles, low first. Unused events read FF 80 00, as a D-20 sends them.
std::vector<uint8_t> encodeD20Pattern(const D20Pattern& pattern);
// DT1 messages (to `deviceId`, 10H = unit #17) that write patterns to P-51-P-88 (0A 00 00 on): patterns[n] (0-31) goes
// to P-51 + n, in messages of at most 256 bytes as the D-20's own dumps; ones not present are left out. d110emu's
// Patterns tab loads such a file as its presets P-11-P-48 (Load presets).
std::vector<uint8_t> d20PatternDump(const std::array<D20Pattern, 32>& patterns, uint8_t deviceId = 0x10);
// The patterns P-51-P-88 in DT1 messages to the D-20 (such a dump, or a D-20 bulk dump); `found` counts those present.
std::array<D20Pattern, 32> readD20PatternDump(const uint8_t* data, size_t length, int* found = nullptr);
// The names the D-20 shows for its preset patterns P-11-P-48 (its owner's manual, p.25), and shortened to 9 characters
// for small buttons.
extern const char* const kD20PresetPatternNames[kD20PresetPatterns];
extern const char* const kD20PresetPatternShortNames[kD20PresetPatterns];

std::string d20PatternName(int pattern);  // 0-63 -> "P-11".."P-88"
std::string d20TrackEntryName(int entry); // "P-51", or "Blank 4/4"
// Length of a track bar in beats: its pattern's, 4 for a pattern that is not loaded.
int d20BarBeats(const D20Rhythm& rhythm, int entry);

// MIDI notes on `channel` at `bpm`, each with a note-off a 16th later (the drum tones mostly ignore it). A pattern
// plays its one bar (loop it with the player); the track plays its bars in turn. `barStarts` receives the start of
// every bar, in seconds.
std::unique_ptr<SmfFile> d20PatternToSmf(const D20Rhythm& rhythm, int pattern, int channel, double bpm);
std::unique_ptr<SmfFile> d20TrackToSmf(const D20Rhythm& rhythm, int channel, double bpm, std::vector<double>* barStarts = nullptr);
