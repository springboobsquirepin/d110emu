#include "PatternCapture.h"

#include <algorithm>
#include <tuple>

namespace {

constexpr double kMaxClockInterval = 0.2;  // Seconds; the D-20's slowest tempo (20 BPM) clocks every 0.125 s

}  // namespace

void PatternCapture::setChannel(int channel) {
    channel_ = std::clamp(channel, 0, 15);
}

void PatternCapture::onMessage(uint32_t message, double seconds) {
    const uint8_t status = uint8_t(message & 0xFF);
    switch (status) {
    case 0xF8:  // Timing clock
        if (lastClock_ >= 0.0 && seconds - lastClock_ > 0.0 && seconds - lastClock_ < kMaxClockInterval) {
            recentIntervals_.push_back(seconds - lastClock_);
            if (recentIntervals_.size() > size_t(kD20StepsPerQuarter)) recentIntervals_.erase(recentIntervals_.begin());
        }
        lastClock_ = seconds;
        clockSeen_ = true;
        if (playing_) clockTimes_.push_back(seconds);
        return;
    case 0xFA:  // Start: from the pattern's first step
    case 0xFB:  // Continue: from where it stopped
        playing_ = true;
        aligned_ = status == 0xFA;
        finished_ = false;
        clockTimes_.clear();
        notes_.clear();
        return;
    case 0xFC:  // Stop
        finished_ = playing_;
        playing_ = false;
        return;
    default:
        break;
    }
    if (!playing_ || (status & 0xF0) != 0x90 || (status & 0x0F) != channel_) return;
    const uint8_t velocity = uint8_t((message >> 16) & 0x7F);
    if (velocity > 0) notes_.push_back({seconds, uint8_t((message >> 8) & 0x7F), velocity});
}

double PatternCapture::clockInterval() const {
    if (recentIntervals_.empty()) return 0.0;
    double sum = 0.0;
    for (double interval : recentIntervals_) sum += interval;
    return sum / double(recentIntervals_.size());
}

double PatternCapture::bpm() const {
    const double interval = clockInterval();
    return interval > 0.0 ? 60.0 / (interval * kD20StepsPerQuarter) : 0.0;
}

PatternCapture::Take PatternCapture::take() const {
    Take take;
    take.aligned = aligned_;
    take.clocks = int(clockTimes_.size());
    take.bpm = bpm();
    const double interval = clockInterval();
    for (const RawNote& note : notes_) {
        // The nearest clock; after the last one, the next (not received yet) if the note is nearer to it.
        size_t clock = 0;
        if (!clockTimes_.empty()) {
            clock = size_t(std::lower_bound(clockTimes_.begin(), clockTimes_.end(), note.time) - clockTimes_.begin());
            if (clock >= clockTimes_.size()) {
                clock = clockTimes_.size() - 1;
                if (interval > 0.0 && note.time - clockTimes_[clock] > interval * 0.5) clock++;
            } else if (clock > 0 && note.time - clockTimes_[clock - 1] < clockTimes_[clock] - note.time) {
                clock--;
            }
        }
        if (note.key < 24 || note.key > 108) {
            take.outOfRange++;
            continue;
        }
        take.notes.push_back({int(clock), note.key, note.velocity});
    }
    return take;
}

bool PatternCapture::takeFinished() {
    const bool finished = finished_;
    finished_ = false;
    return finished;
}

int PatternCapture::wholeBars(const Take& take, int beats) {
    return take.clocks / (std::clamp(beats, 1, 8) * kD20StepsPerQuarter);
}

std::vector<int> PatternCapture::repeatingBeats(const Take& take) {
    std::vector<int> repeating;
    for (int beats = 1; beats <= 8; beats++) {
        const int length = beats * kD20StepsPerQuarter;
        const int bars = wholeBars(take, beats);
        if (bars < 2) continue;
        std::vector<std::vector<std::tuple<int, int, int>>> perBar(static_cast<size_t>(bars));
        for (const D20Pattern::Note& note : take.notes) {
            if (note.step < bars * length) perBar[size_t(note.step / length)].emplace_back(note.step % length, note.key, note.velocity);
        }
        for (auto& bar : perBar) std::sort(bar.begin(), bar.end());
        if (std::all_of(perBar.begin(), perBar.end(), [&](const auto& bar) { return bar == perBar[0]; })) repeating.push_back(beats);
    }
    return repeating;
}

int PatternCapture::suggestedBeats(const Take& take) {
    const std::vector<int> repeating = repeatingBeats(take);
    if (repeating.empty() || std::find(repeating.begin(), repeating.end(), 4) != repeating.end()) return 4;
    return repeating.front();
}

D20Pattern PatternCapture::pattern(const Take& take, int beats) {
    D20Pattern pattern;
    pattern.beats = std::clamp(beats, 1, 8);
    pattern.present = true;
    const int length = pattern.beats * kD20StepsPerQuarter;
    for (const D20Pattern::Note& note : take.notes) {
        if (note.step < length) pattern.notes.push_back(note);
    }
    std::stable_sort(pattern.notes.begin(), pattern.notes.end(),
                     [](const D20Pattern::Note& a, const D20Pattern::Note& b) { return a.step < b.step; });
    if (pattern.notes.size() > 96) pattern.notes.resize(96);
    return pattern;
}
