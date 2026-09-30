#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "SmfFile.h"

// A test sequence for measuring the reverb of a real D-110 or D-10/D-20, and of d110emu, which plays the same file:
// short noise bursts through every reverb type and Reverb Time, a sweep of the Reverb Level, dry bursts to align and
// subtract, and short phrases to compare by ear. tools/reverb_analysis.py measures recordings of it.
// - D-110: part 1 on MIDI channel 1 plays the burst (output Mix + reverb); the reverb is set in the system area.
// - D-10/D-20 in performance mode (receive channel 1): the performance patch holds the reverb, its upper tone the burst.
// Both files have the same timing, so their recordings (and d110emu's render) line up segment by segment.
enum class ReverbKitTarget { D110, D20 };

struct ReverbKitSegment {
    double start = 0.0;  // Seconds from the start of the file, where the burst or phrase begins
    std::string kind;    // "dry" (reverb off), "burst" or "phrase"
    int type = 8;        // 0-7 = types 1-8, 8 = off
    int time = 1;        // Reverb Time 1-8
    int level = 0;       // Reverb Level 0-7
};

// The sequence, setup first (spaced for a real unit's MIDI input), and its segments.
std::unique_ptr<SmfFile> reverbKitSmf(ReverbKitTarget target, std::vector<ReverbKitSegment>& schedule);
// "start,kind,type,time,level" lines, with a header.
std::string reverbKitScheduleCsv(const std::vector<ReverbKitSegment>& schedule);
// Writes reverb-test-d110.mid, reverb-test-d20.mid, reverb-test-schedule.csv and README.txt into `folder`.
bool writeReverbKit(const std::filesystem::path& folder, std::string& error);
