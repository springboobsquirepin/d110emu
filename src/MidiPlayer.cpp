#include "MidiPlayer.h"

#include <algorithm>
#include <cmath>

#include "mt32emu.h"

namespace {

// Songs shorter than this never loop, so a near-empty file cannot spin the audio thread.
constexpr double kMinLoopDuration = 0.05;

// Wrap-safe "a is before b" for the synth's 32-bit sample counter.
bool before(uint32_t a, uint32_t b) {
    return int32_t(a - b) < 0;
}

}  // namespace

void MidiPlayer::load(std::unique_ptr<SmfFile> file, std::string name) {
    file_ = std::move(file);
    name_ = std::move(name);
    queueNext(nullptr, std::string());
    state_ = file_ ? State::Stopped : State::Empty;
    speed_ = 1.0;
    next_ = 0;
    anchorSong_ = 0.0;
    pausedAt_ = 0.0;
}

void MidiPlayer::play(uint32_t synthNow) {
    if (state_ == State::Empty || state_ == State::Playing) return;
    anchorSong_ = state_ == State::Paused ? pausedAt_ : 0.0;
    const auto& events = file_->events;
    next_ = size_t(std::lower_bound(events.begin(), events.end(), anchorSong_,
                                    [](const SmfEvent& e, double t) { return e.time < t; }) - events.begin());
    anchorSynth_ = synthNow;
    state_ = State::Playing;
}

void MidiPlayer::pause(MT32Emu::Synth& synth, uint32_t synthNow) {
    if (state_ != State::Playing) return;
    pausedAt_ = position(synthNow);
    state_ = State::Paused;
    silence(synth);
}

void MidiPlayer::stop(MT32Emu::Synth* synth) {
    queueNext(nullptr, std::string());
    if (state_ == State::Empty) return;
    const bool wasActive = state_ == State::Playing || state_ == State::Paused;
    state_ = State::Stopped;
    next_ = 0;
    anchorSong_ = 0.0;
    pausedAt_ = 0.0;
    if (wasActive && synth != nullptr) silence(*synth);
}

void MidiPlayer::queueNext(std::unique_ptr<SmfFile> file, std::string name) {
    queued_ = std::move(file);
    queuedName_ = queued_ ? std::move(name) : std::string();
}

void MidiPlayer::setSpeed(double speed, uint32_t synthNow) {
    speed = std::clamp(speed, 0.05, 20.0);
    if (state_ == State::Playing) {
        // Continue from the current position at the new speed.
        anchorSong_ = position(synthNow);
        anchorSynth_ = synthNow;
    }
    speed_ = speed;
}

double MidiPlayer::position(uint32_t synthNow) const {
    switch (state_) {
    case State::Playing: {
        const double t = anchorSong_ + double(int32_t(synthNow - anchorSynth_)) / MT32Emu::SAMPLE_RATE * speed_;
        return std::clamp(t, 0.0, duration());
    }
    case State::Paused:
        return pausedAt_;
    default:
        return 0.0;
    }
}

uint32_t MidiPlayer::toSynthTime(double songTime) const {
    return anchorSynth_ + uint32_t(int64_t(std::llround((songTime - anchorSong_) / speed_ * MT32Emu::SAMPLE_RATE)));
}

void MidiPlayer::pump(uint32_t windowEnd, const PlayFunction& play) {
    if (state_ != State::Playing) return;
    for (;;) {
        const auto& events = file_->events;  // The file changes when a queued one takes over
        if (next_ >= events.size()) {
            const uint32_t songEnd = toSynthTime(duration());
            if (!before(songEnd, windowEnd)) return;  // Wait until the song end is reached
            if (queued_) {
                // The queued file starts where this one ends, as a loop would.
                file_ = std::move(queued_);
                name_ = std::move(queuedName_);
                queuedName_.clear();
                anchorSynth_ = songEnd;
                anchorSong_ = 0.0;
                next_ = 0;
                continue;
            }
            if (loop_ && duration() >= kMinLoopDuration) {
                anchorSynth_ = songEnd;
                anchorSong_ = 0.0;
                next_ = 0;
                continue;
            }
            state_ = State::Stopped;
            next_ = 0;
            anchorSong_ = 0.0;
            return;
        }
        const SmfEvent& e = events[next_];
        const uint32_t timestamp = toSynthTime(e.time);
        if (!before(timestamp, windowEnd)) return;
        const bool queued = e.shortMessage != 0 ? play(e.shortMessage, nullptr, 0, timestamp)
                                                : play(0, &file_->sysexData[e.sysexOffset], e.sysexLength, timestamp);
        if (!queued) return;  // Queue full: retry on the next block
        next_++;
    }
}

void MidiPlayer::silence(MT32Emu::Synth& synth) {
    synth.flushMIDIQueue();
    for (uint32_t channel = 0; channel < 16; channel++) {
        synth.playMsgNow(0xB0 | channel | (64 << 8));   // Sustain off
        synth.playMsgNow(0xB0 | channel | (123 << 8));  // All notes off
    }
}
