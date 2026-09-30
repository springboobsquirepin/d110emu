#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "SmfFile.h"

namespace MT32Emu {
class Synth;
}

// Feeds a loaded MIDI file into the synth's MIDI queue with sample-accurate timestamps.
// Not thread-safe: SynthEngine calls every method with its lock held, and pump() runs on
// the audio thread right before each block is rendered.
class MidiPlayer {
public:
    enum class State { Empty, Stopped, Playing, Paused };

    void load(std::unique_ptr<SmfFile> file, std::string name);
    void play(uint32_t synthNow);
    void pause(MT32Emu::Synth& synth, uint32_t synthNow);
    // Rewinds. Pass the synth to also silence notes that are still sounding.
    void stop(MT32Emu::Synth* synth);
    void setLoop(bool loop) { loop_ = loop; }
    // Playback speed (1 = as written, 2 = twice as fast), taking effect at once; load() resets it to 1.
    void setSpeed(double speed, uint32_t synthNow);
    double speed() const { return speed_; }
    // Plays `file` next: when the current file reaches its end (also where it would loop), the queued one takes over
    // from its start at that very sample, at the same speed, and the queue empties. A later call replaces the queued
    // file, nullptr clears it; load() and stop() clear it too.
    void queueNext(std::unique_ptr<SmfFile> file, std::string name);
    const std::string& queuedName() const { return queuedName_; }  // Empty when nothing is queued

    // Queues an event for the synth at `timestamp`: a short message, or SysEx when shortMessage is 0. Returns false
    // when the synth's queue is full.
    using PlayFunction = std::function<bool(uint32_t shortMessage, const uint8_t* sysex, uint32_t sysexLength, uint32_t timestamp)>;
    // Enqueues every event that falls before windowEnd (in synth samples) through `play`.
    void pump(uint32_t windowEnd, const PlayFunction& play);

    State state() const { return state_; }
    bool loop() const { return loop_; }
    double position(uint32_t synthNow) const;
    double duration() const { return file_ ? file_->duration : 0.0; }
    const std::string& name() const { return name_; }

    // Processes everything already queued, then releases all notes and the sustain pedal.
    static void silence(MT32Emu::Synth& synth);

private:
    uint32_t toSynthTime(double songTime) const;

    std::unique_ptr<SmfFile> file_;
    std::string name_;
    std::unique_ptr<SmfFile> queued_;
    std::string queuedName_;
    State state_ = State::Empty;
    bool loop_ = false;
    double speed_ = 1.0;
    size_t next_ = 0;
    // Song time anchorSong_ plays at synth time anchorSynth_.
    uint32_t anchorSynth_ = 0;
    double anchorSong_ = 0.0;
    double pausedAt_ = 0.0;
};
