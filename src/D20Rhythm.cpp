#include "D20Rhythm.h"

#include <algorithm>

namespace {

constexpr int kMaxNotes = 96;
constexpr int kMaxBars = 500;

D20Pattern parsePattern(const uint8_t* nibbles) {
    D20Pattern pattern;
    pattern.present = std::any_of(nibbles, nibbles + kD20PatternSize, [](uint8_t b) { return b != 0; });
    if (!pattern.present) return pattern;
    // Each byte is sent as two nibbles, low first.
    auto byteAt = [&](size_t i) { return (nibbles[2 * i] & 0x0F) | ((nibbles[2 * i + 1] & 0x0F) << 4); };
    pattern.beats = (byteAt(0) & 7) + 1;
    const int count = std::min(byteAt(1), kMaxNotes);
    for (int i = 0; i < count; i++) {
        // Byte 2 is a dummy; the notes (step, key, velocity) follow.
        const D20Pattern::Note note{byteAt(3 + 3 * size_t(i)), byteAt(4 + 3 * size_t(i)), byteAt(5 + 3 * size_t(i))};
        if (note.step < pattern.beats * kD20StepsPerQuarter && note.key >= 24 && note.key <= 108 && note.velocity >= 1 &&
            note.velocity <= 127) {
            pattern.notes.push_back(note);
        }
    }
    std::stable_sort(pattern.notes.begin(), pattern.notes.end(), [](const D20Pattern::Note& a, const D20Pattern::Note& b) {
        return a.step < b.step;
    });
    return pattern;
}

// Collects notes, then sorts them into an SmfFile (note-offs first at equal times, so a repeated key sounds again).
class SmfBuilder {
public:
    SmfBuilder(int channel, double bpm) : channel_(channel & 15), stepSeconds_(60.0 / bpm / kD20StepsPerQuarter) {}

    void addBar(const D20Pattern& pattern, double barStart) {
        const double barEnd = barStart + pattern.beats * kD20StepsPerQuarter * stepSeconds_;
        for (const D20Pattern::Note& note : pattern.notes) {
            const double on = barStart + note.step * stepSeconds_;
            const double off = std::min(on + 6 * stepSeconds_, barEnd);  // A 16th note, within the bar
            events_.push_back({on, 1, uint32_t(0x90 | channel_) | (uint32_t(note.key) << 8) | (uint32_t(note.velocity) << 16)});
            events_.push_back({off, 0, uint32_t(0x80 | channel_) | (uint32_t(note.key) << 8)});
        }
    }

    double barSeconds(int beats) const { return beats * kD20StepsPerQuarter * stepSeconds_; }

    std::unique_ptr<SmfFile> finish(double duration, const std::string& title) {
        std::stable_sort(events_.begin(), events_.end(), [](const Event& a, const Event& b) {
            return a.time != b.time ? a.time < b.time : a.order < b.order;
        });
        std::unique_ptr<SmfFile> smf(new SmfFile);
        for (const Event& e : events_) smf->events.push_back(SmfEvent{e.time, e.message, 0, 0});
        smf->duration = duration;
        smf->trackCount = 1;
        smf->title = title;
        return smf;
    }

private:
    struct Event {
        double time;
        int order;  // 0 note-off, 1 note-on
        uint32_t message;
    };
    int channel_;
    double stepSeconds_;
    std::vector<Event> events_;
};

}  // namespace

D20Rhythm parseD20Rhythm(const uint8_t* patterns, const uint8_t* track) {
    D20Rhythm rhythm;
    for (int i = 0; i < kD20PatternCount; i++) rhythm.patterns[size_t(i)] = parsePattern(patterns + size_t(i) * kD20PatternSize);
    const int length = std::min((track[0] & 0x7F) | ((track[1] & 0x03) << 7), kMaxBars);
    for (int bar = 0; bar < length; bar++) {
        const int entry = track[2 + bar] & 0x7F;
        rhythm.track.push_back(entry <= kD20TrackBlank + 7 ? entry : kD20TrackBlank + 3);  // Anything else: a 4/4 rest
    }
    return rhythm;
}

std::vector<uint8_t> encodeD20Pattern(const D20Pattern& pattern) {
    constexpr size_t kBytes = kD20PatternSize / 2;
    uint8_t bytes[kBytes] = {};
    const size_t count = std::min<size_t>(pattern.notes.size(), size_t(kMaxNotes));
    bytes[0] = uint8_t(std::clamp(pattern.beats, 1, 8) - 1);
    bytes[1] = uint8_t(count);
    for (size_t i = 0; i < size_t(kMaxNotes); i++) {
        uint8_t* event = &bytes[3 + 3 * i];
        if (i < count) {
            const D20Pattern::Note& note = pattern.notes[i];
            event[0] = uint8_t(std::clamp(note.step, 0, 191));
            event[1] = uint8_t(std::clamp(note.key, 0, 127));
            event[2] = uint8_t(std::clamp(note.velocity, 1, 127));
        } else {
            event[0] = 0xFF;  // An unused event, as a D-20 sends it
            event[1] = 0x80;
            event[2] = 0x00;
        }
    }
    bytes[3 + 3 * kMaxNotes] = 0xFF;  // End mark; the two dummies after it stay 0
    std::vector<uint8_t> nibbles(kD20PatternSize);
    for (size_t i = 0; i < kBytes; i++) {
        nibbles[2 * i] = bytes[i] & 0x0F;
        nibbles[2 * i + 1] = bytes[i] >> 4;
    }
    return nibbles;
}

std::vector<uint8_t> d20PatternDump(const std::array<D20Pattern, 32>& patterns, uint8_t deviceId) {
    constexpr uint32_t kFirst = (0x0A << 14);  // P-51 at 0A 00 00, packed 7-bit address
    constexpr size_t kMessageData = 256;
    std::vector<uint8_t> out;
    for (size_t n = 0; n < patterns.size(); n++) {
        if (!patterns[n].present) continue;
        const std::vector<uint8_t> data = encodeD20Pattern(patterns[n]);
        for (size_t offset = 0; offset < data.size(); offset += kMessageData) {
            const size_t length = std::min(kMessageData, data.size() - offset);
            const uint32_t address = kFirst + uint32_t(n * kD20PatternSize + offset);
            const uint8_t header[8] = {0xF0, 0x41, deviceId, 0x16, 0x12, uint8_t(address >> 14), uint8_t((address >> 7) & 0x7F),
                                       uint8_t(address & 0x7F)};
            out.insert(out.end(), header, header + 8);
            out.insert(out.end(), data.begin() + long(offset), data.begin() + long(offset + length));
            unsigned sum = header[5] + header[6] + header[7];
            for (size_t i = 0; i < length; i++) sum += data[offset + i];
            out.push_back(uint8_t((128 - sum % 128) % 128));
            out.push_back(0xF7);
        }
    }
    return out;
}

std::array<D20Pattern, 32> readD20PatternDump(const uint8_t* data, size_t length, int* found) {
    // Collected where parseD20Rhythm() expects P-51-P-88, after 32 blank presets.
    constexpr uint32_t kFirst = (0x0A << 14);
    constexpr uint32_t kSize = uint32_t(kD20PresetPatterns * kD20PatternSize);
    std::vector<uint8_t> memory(size_t(kD20PatternCount) * kD20PatternSize, 0);
    for (size_t i = 0; i < length; i++) {
        if (data[i] != 0xF0) continue;
        size_t end = i + 1;
        while (end < length && data[end] != 0xF7) end++;
        if (end >= length) break;
        const uint8_t* message = &data[i];
        const size_t size = end + 1 - i;
        i = end;
        if (size < 11 || message[1] != 0x41 || message[3] != 0x16 || message[4] != 0x12) continue;
        const uint32_t address = (uint32_t(message[5]) << 14) | (uint32_t(message[6]) << 7) | message[7];
        for (size_t k = 0; k + 10 < size; k++) {
            const uint32_t at = address + uint32_t(k);
            if (at >= kFirst && at < kFirst + kSize) memory[size_t(kD20PresetPatterns) * kD20PatternSize + (at - kFirst)] = message[8 + k] & 0x7F;
        }
    }
    const uint8_t track[kD20RhythmTrackSize] = {};
    const D20Rhythm rhythm = parseD20Rhythm(memory.data(), track);
    std::array<D20Pattern, 32> patterns;
    int present = 0;
    for (size_t n = 0; n < patterns.size(); n++) {
        patterns[n] = rhythm.patterns[size_t(kD20PresetPatterns) + n];
        if (patterns[n].present) present++;
    }
    if (found != nullptr) *found = present;
    return patterns;
}

const char* const kD20PresetPatternNames[kD20PresetPatterns] = {
    "8Beat 1", "8Beat 2", "8Beat 3", "8Beat 4", "8Beat 5", "8Beat 6", "Ballad", "Reggae",
    "16Beat 1", "16Beat 2", "16Beat 3", "16Beat 4", "16Beat 5", "16Beat 6", "Shuffle 1", "Shuffle 2",
    "Disco 1", "Disco 2", "Electric Pop 1", "Electric Pop 2", "Jazz 1", "Jazz 2", "Jazz 3", "Jazz Waltz",
    "Samba 1", "Samba 2", "Samba 3", "Bossanova 1", "Bossanova 2", "Mambo", "Merengue", "Rumba",
};

const char* const kD20PresetPatternShortNames[kD20PresetPatterns] = {
    "8Beat 1", "8Beat 2", "8Beat 3", "8Beat 4", "8Beat 5", "8Beat 6", "Ballad", "Reggae",
    "16Beat 1", "16Beat 2", "16Beat 3", "16Beat 4", "16Beat 5", "16Beat 6", "Shuffle 1", "Shuffle 2",
    "Disco 1", "Disco 2", "El.Pop 1", "El.Pop 2", "Jazz 1", "Jazz 2", "Jazz 3", "JazzWaltz",
    "Samba 1", "Samba 2", "Samba 3", "Bossa 1", "Bossa 2", "Mambo", "Merengue", "Rumba",
};

std::string d20PatternName(int pattern) {
    pattern &= 63;
    return std::string("P-") + char('1' + pattern / 8) + char('1' + pattern % 8);
}

std::string d20TrackEntryName(int entry) {
    if (entry < kD20TrackBlank) return d20PatternName(entry);
    return std::string("Blank ") + char('1' + ((entry - kD20TrackBlank) & 7)) + "/4";
}

int d20BarBeats(const D20Rhythm& rhythm, int entry) {
    if (entry >= kD20TrackBlank) return entry - kD20TrackBlank + 1;
    const D20Pattern& pattern = rhythm.patterns[size_t(entry & 63)];
    return pattern.present ? pattern.beats : 4;
}

std::unique_ptr<SmfFile> d20PatternToSmf(const D20Rhythm& rhythm, int pattern, int channel, double bpm) {
    SmfBuilder builder(channel, bpm);
    const D20Pattern& p = rhythm.patterns[size_t(pattern & 63)];
    builder.addBar(p, 0.0);
    return builder.finish(builder.barSeconds(p.present ? p.beats : 4), "D-20 " + d20PatternName(pattern));
}

std::unique_ptr<SmfFile> d20TrackToSmf(const D20Rhythm& rhythm, int channel, double bpm, std::vector<double>* barStarts) {
    SmfBuilder builder(channel, bpm);
    double time = 0.0;
    if (barStarts != nullptr) barStarts->clear();
    for (int entry : rhythm.track) {
        if (barStarts != nullptr) barStarts->push_back(time);
        if (entry < kD20TrackBlank) builder.addBar(rhythm.patterns[size_t(entry)], time);
        time += builder.barSeconds(d20BarBeats(rhythm, entry));
    }
    return builder.finish(time, "D-20 rhythm track");
}
