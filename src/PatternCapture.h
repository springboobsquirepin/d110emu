#pragma once

#include <cstdint>
#include <vector>

#include "D20Rhythm.h"

// Captures a D-20 rhythm pattern from its MIDI OUT. In the Rhythm mode's Pattern Play with Clock Mode INTERNAL, a
// D-20 sends the pattern's notes on the rhythm part's MIDI channel, and MIDI clock: 24 clocks per quarter note, the
// patterns' own step grid, so every note falls on a clock. Holding STOP and pressing START sends Start (FAH) and plays
// from the pattern's first step; START alone sends Continue (FBH), which resumes where it stopped; STOP sends Stop
// (FCH). (The D-20's MIDI implementation, section 2.)
//
// A take runs from Start (or Continue) to Stop. Each note goes to the clock it arrived nearest to, clock 0 being the
// first after Start (the D-20 sends a step's clock and notes within a millisecond or two, the clocks 10-60 ms apart).
// The pattern is the take's first bar; its other whole bars show which bar lengths the notes repeat at.
class PatternCapture {
public:
    struct Take {
        bool aligned = true;   // Began with Start; after Continue the first clock need not be the pattern's first step
        int clocks = 0;        // Clocks received: the take's length in steps
        double bpm = 0.0;      // Measured from the clock
        std::vector<D20Pattern::Note> notes;  // In arrival order; step = the clock since the start
        int outOfRange = 0;    // Notes outside the patterns' keys (24-108), left out
    };

    void setChannel(int channel);  // 0-15: the D-20's rhythm part (MIDI channel 10 by default)
    int channel() const { return channel_; }

    // A message from the D-20: a short message (note on and others) or a real-time status byte (F8H clock, FAH start,
    // FBH continue, FCH stop), with its arrival time in seconds (any origin; only differences count).
    void onMessage(uint32_t message, double seconds);

    bool playing() const { return playing_; }
    bool hasClock() const { return clockSeen_; }
    int clocks() const { return int(clockTimes_.size()); }  // Of the take being recorded, or the last one
    double bpm() const;                                      // From the last beat of clocks, 0 before two clocks
    // The take being recorded, or the last one (from Start or Continue to Stop).
    Take take() const;
    // A take has ended with Stop since the last call.
    bool takeFinished();

    // Bar lengths (1-8 beats) that the take's notes repeat at, over at least two whole bars.
    static std::vector<int> repeatingBeats(const Take& take);
    // 4 if the notes repeat at 4/4 (most patterns; a simple beat also repeats every half bar), else the shortest
    // length they repeat at, else 4.
    static int suggestedBeats(const Take& take);
    // The take's first bar of `beats` as a pattern: at most 96 notes (the D-20's limit), in step order.
    static D20Pattern pattern(const Take& take, int beats);
    // Whole bars of `beats` in the take.
    static int wholeBars(const Take& take, int beats);

private:
    struct RawNote {
        double time;
        uint8_t key;
        uint8_t velocity;
    };
    double clockInterval() const;  // Seconds per clock, from the latest clocks

    int channel_ = 9;
    bool playing_ = false;
    bool aligned_ = true;
    bool finished_ = false;
    bool clockSeen_ = false;
    std::vector<double> clockTimes_;  // Of the take, from its first clock
    std::vector<RawNote> notes_;
    double lastClock_ = -1.0;         // Any clock, also outside a take (the D-20 may clock while stopped)
    std::vector<double> recentIntervals_;
};
