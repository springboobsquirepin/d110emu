#include "RomPlay.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>

namespace {

constexpr size_t kNameSize = 16;
constexpr size_t kHeaderSize = 16;
constexpr size_t kRhythmSetupSize = 64 * 4;
constexpr size_t kFirmwareSize = 0x8000;      // IC19; the data ROM (IC12) follows it in the image
constexpr size_t kToneCommonSize = 14;
constexpr size_t kTonePartialSize = 58;
constexpr size_t kToneSize = kToneCommonSize + 4 * kTonePartialSize;
constexpr uint8_t kLongWait = 0xF8;           // In place of a delta: wait 248 ticks, then read another delta
constexpr uint8_t kEndMark = 0xFC;            // In place of a message: end of song
constexpr uint8_t kDemoToneGroup = 5;
constexpr double kTimerCountSeconds = 2e-6;   // Timer1 of the 12 MHz CPU counts every 2 us
constexpr uint16_t kDefaultTickPeriod = 0x1816;
// 4AH = 442 Hz: the songs were made with pre-release firmware, whose default that was (the released units' is 440 Hz).
constexpr uint8_t kRomPlayMasterTune = 0x4A;
constexpr unsigned int kFirstPartChannel = 1;  // MIDI channel 2
constexpr unsigned int kRhythmChannel = 9;     // MIDI channel 10

// Linear SysEx addresses (7 bits per address byte).
constexpr uint32_t kSystemAddress = 0x10 << 14;
constexpr uint32_t kPartTempAddress = 0x03 << 14;          // + 10H per part, rhythm part last
constexpr uint32_t kRhythmSetupAddress = (0x03 << 14) + 0x90;  // 03 01 10: key 24
constexpr uint32_t kToneTempAddress = 0x04 << 14;          // + F6H per part
constexpr size_t kPresetToneMaps[2] = {0x8000, 0x8080};    // Groups a and b
constexpr size_t kPresetToneOffset = 0x8000;

constexpr double kPlaybackLeadIn = 0.1;
constexpr double kChainGapSeconds = 2.0;       // Pause between songs when chained
constexpr double kMidiBytesPerSecond = 3125.0;
constexpr double kHardwareSysexGap = 0.04;     // Rest after each setup SysEx so that a real unit keeps up
constexpr uint16_t kExportTicksPerQuarter = 480;

// Tempos measured from the note timing of the D-110 v1.10 songs: the length of a quarter note in ticks, and
// the tick of the first bar line (the first note). Only used to give exported files a matching tempo and bar
// grid; playback timing comes from the ticks alone. Sugar Plum and Dinner Set change tempo along the way.
struct SongTempo {
    const char* name;
    double beatTicks;
    double downbeatTick;
};
constexpr SongTempo kSongTempos[] = {
    {"Macho Memory", 28.866, 27.34},  // 168.6 BPM
    {"Jah May Kah!", 31.703, 14.17},  // 153.5 BPM
    {"Sugar Plum", 83.464, 51.0},     // 58.3 BPM
    {"My Brother", 32.325, 31.13},    // 150.5 BPM
    {"Folk", 44.084, 20.10},          // 110.4 BPM
    {"Bumble Dee", 30.898, 13.76},    // 157.5 BPM
    {"Mergatroid", 48.984, 47.54},    // 99.3 BPM
    {"Dinner Set", 53.901, -1.47},    // 90.3 BPM
};

bool isNameChar(uint8_t c) {
    return c >= 0x20 && c < 0x7F;
}

uint16_t readWord(const std::vector<uint8_t>& rom, size_t at) {
    return uint16_t(rom[at] | (rom[at + 1] << 8));
}

// Finds a byte sequence in the firmware; -1 matches any byte. Returns the offset or -1.
long findCode(const std::vector<uint8_t>& rom, std::initializer_list<int> pattern) {
    const size_t end = std::min(rom.size(), kFirmwareSize);
    for (size_t i = 0; i + pattern.size() <= end; i++) {
        size_t matched = 0;
        for (int byte : pattern) {
            if (byte >= 0 && rom[i + matched] != uint8_t(byte)) break;
            matched++;
        }
        if (matched == pattern.size()) return long(i);
    }
    return -1;
}

// Walks the event stream from `start`; returns the offset just past the end mark, or 0 if the data is malformed.
size_t scanEvents(const std::vector<uint8_t>& rom, size_t start) {
    size_t p = start;
    uint8_t runningStatus = 0;
    while (p < rom.size()) {
        const uint8_t delta = rom[p++];
        if (delta == kLongWait) continue;
        if (p >= rom.size()) return 0;
        uint8_t status = rom[p];
        if (status == kEndMark) return p + 1;
        if (status >= 0xF0) return 0;
        if (status >= 0x80) {
            runningStatus = status;
            p++;
        } else if (runningStatus == 0) {
            return 0;
        }
        const uint8_t type = runningStatus & 0xF0;
        const size_t dataBytes = (type == 0xC0 || type == 0xD0) ? 1 : 2;
        for (size_t i = 0; i < dataBytes; i++) {
            if (p >= rom.size() || rom[p] >= 0x80) return 0;
            p++;
        }
    }
    return 0;
}

// Reads the song at `offset` (name, header, rhythm setup, events); false if it does not look like one.
bool readSong(const std::vector<uint8_t>& rom, size_t offset, RomSong& song, size_t& end) {
    const size_t eventsStart = offset + kNameSize + kHeaderSize + kRhythmSetupSize;
    if (eventsStart >= rom.size()) return false;
    const uint8_t* data = &rom[offset];
    if (data[0] == ' ' || !std::all_of(data, data + kNameSize, isNameChar)) return false;
    end = scanEvents(rom, eventsStart);
    if (end == 0) return false;
    song.name.assign(reinterpret_cast<const char*>(data), kNameSize);
    song.name.erase(song.name.find_last_not_of(' ') + 1);
    // The firmware loads the first header word into the period of the timer that counts song ticks.
    uint16_t period = uint16_t(data[kNameSize] | (data[kNameSize + 1] << 8));
    if (period < 1000 || period > 50000) period = kDefaultTickPeriod;
    song.tickSeconds = period * kTimerCountSeconds;
    std::memcpy(song.reverb, data + kNameSize + 2, 3);
    std::memcpy(song.rhythmSetup, data + kNameSize + kHeaderSize, kRhythmSetupSize);
    song.events.assign(rom.begin() + long(eventsStart), rom.begin() + long(end));
    return true;
}

// Songs listed in the firmware's song table (bank and word offset per song, IC12 banks of 16 KiB).
std::vector<RomSong> readSongTable(const std::vector<uint8_t>& rom) {
    std::vector<RomSong> songs;
    // LDB R75,#20 (bank); SCALL; LD R70,table[R76]; RET
    const long code = findCode(rom, {0xB1, 0x20, 0x75, -1, -1, 0xA3, 0x77, -1, -1, 0x70, 0xF0});
    if (code < 0) return songs;
    const size_t table = readWord(rom, size_t(code) + 7);
    for (size_t i = 0; i < 16 && table + 2 * i + 2 <= rom.size(); i++) {
        const uint16_t pointer = readWord(rom, table + 2 * i);
        const size_t offset = kFirmwareSize + size_t(pointer >> 13) * 0x4000 + (size_t(pointer & 0x1FFF) << 1);
        RomSong song;
        size_t end = 0;
        if (!readSong(rom, offset, song, end)) break;
        songs.push_back(std::move(song));
    }
    return songs;
}

// Fallback: songs found by their header signature (tick period 1816H).
std::vector<RomSong> scanForSongs(const std::vector<uint8_t>& rom) {
    std::vector<RomSong> songs;
    for (size_t offset = 0; offset + kNameSize + 2 < rom.size();) {
        RomSong song;
        size_t end = 0;
        if (rom[offset + kNameSize] == 0x16 && rom[offset + kNameSize + 1] == 0x18 && readSong(rom, offset, song, end)) {
            songs.push_back(std::move(song));
            offset = end;
        } else {
            offset++;
        }
    }
    return songs;
}

// Unpacks a preset-style tone: partials that are muted are left out of the ROM, except the first, and take
// the data of the partial before them.
bool readTone(const std::vector<uint8_t>& rom, size_t address, std::array<uint8_t, kToneSize>& tone) {
    if (address + kToneCommonSize > rom.size()) return false;
    std::memcpy(tone.data(), &rom[address], kToneCommonSize);
    if (!std::all_of(tone.begin(), tone.begin() + 10, isNameChar)) return false;
    const uint8_t partialMute = tone[12];
    size_t source = address + kToneCommonSize;
    for (size_t partial = 0; partial < 4; partial++) {
        if (partial != 0 && ((partialMute >> partial) & 1) == 0) source -= kTonePartialSize;
        if (source + kTonePartialSize > rom.size()) return false;
        std::memcpy(&tone[kToneCommonSize + partial * kTonePartialSize], &rom[source], kTonePartialSize);
        source += kTonePartialSize;
    }
    return true;
}

// Picks the preset (a or b group) that stands in for a demo tone: the same name, else the closest data.
void findStandIn(const std::vector<uint8_t>& rom, RomPlayTimbres::DemoTone& demo) {
    size_t bestDifference = SIZE_MAX;
    for (uint8_t group = 0; group < 2; group++) {
        for (uint8_t number = 0; number < 64; number++) {
            // Preset tone maps of the D-110 (as in the mt32emu ROM map): pointers relative to 8000H.
            const size_t map = kPresetToneMaps[group] + 2 * number;
            std::array<uint8_t, kToneSize> preset{};
            if (map + 2 > rom.size() || !readTone(rom, kPresetToneOffset + readWord(rom, map), preset)) {
                demo.group = 0xFF;  // No preset tones where expected: keep the part's tone number
                return;
            }
            size_t difference = 0;
            if (std::memcmp(preset.data(), demo.data.data(), 10) != 0) {
                difference = 1;  // Any tone with the same name wins
                for (size_t i = 10; i < kToneSize; i++) difference += preset[i] != demo.data[i];
            }
            if (difference < bestDifference) {
                bestDifference = difference;
                demo.group = group;
                demo.number = number;
            }
        }
    }
}

// The timbre bank that ROM Play program changes use, and the demo tones (group 5) it refers to.
std::shared_ptr<const RomPlayTimbres> readTimbres(const std::vector<uint8_t>& rom) {
    // LDB RB7,#20; STB RB7,0100[0] (bank 0); MULUB R78,R70,#08; ADD R78,#bank
    const long bankCode = findCode(rom, {0xB1, 0x20, 0xB7, 0xC7, 0x01, 0x00, 0x01, 0xB7, 0x5D, 0x08, 0x70, 0x78, 0x65, -1, -1, 0x78});
    // LD R78,pointers[R78]; ADD R78,#8000
    const long toneCode = findCode(rom, {0xA3, 0x79, -1, -1, 0x78, 0x65, 0x00, 0x80, 0x78});
    if (bankCode < 0 || toneCode < 0) return nullptr;
    const size_t bank = readWord(rom, size_t(bankCode) + 13);
    const size_t pointers = readWord(rom, size_t(toneCode) + 2);
    if (bank < kFirmwareSize || bank + 64 * 8 > rom.size() || pointers + 2 > rom.size()) return nullptr;

    std::shared_ptr<RomPlayTimbres> result = std::make_shared<RomPlayTimbres>();
    size_t demoTones = 0;
    for (size_t i = 0; i < 64; i++) {
        std::array<uint8_t, 8>& timbre = result->timbres[i];
        std::memcpy(timbre.data(), &rom[bank + i * 8], 8);
        const bool valid = (timbre[0] <= 3 || timbre[0] == kDemoToneGroup) && timbre[1] < 64 && timbre[2] <= 48 &&
                           timbre[3] <= 100 && timbre[4] <= 24 && timbre[5] <= 3 && timbre[6] <= 7;
        if (!valid) return nullptr;
        if (timbre[0] == kDemoToneGroup) demoTones = std::max(demoTones, size_t(timbre[1]) + 1);
    }
    for (size_t i = 0; i < demoTones; i++) {
        if (pointers + 2 * i + 2 > rom.size()) return nullptr;
        // The firmware adds 8000H to the pointer and maps the result through the IC12 banks.
        const size_t address = kFirmwareSize + ((readWord(rom, pointers + 2 * i) + 0x8000) & 0xFFFF);
        RomPlayTimbres::DemoTone tone;
        if (!readTone(rom, address, tone.data)) return nullptr;
        findStandIn(rom, tone);
        result->tones.push_back(tone);
    }
    return result;
}

// Partial reserves per song (9 bytes each, in song table order).
long findReserveTable(const std::vector<uint8_t>& rom) {
    // MULUB R78,R70,#09; ADD R78,#table; LDB R75,#09
    const long code = findCode(rom, {0x5D, 0x09, 0x70, 0x78, 0x65, -1, -1, 0x78, 0xB1, 0x09, 0x75});
    return code < 0 ? -1 : long(readWord(rom, size_t(code) + 5));
}

uint32_t packAddress(uint32_t linear) {
    return (((linear >> 14) & 0x7F) << 16) | (((linear >> 7) & 0x7F) << 8) | (linear & 0x7F);
}

// Roland DT1 for model 16H; `address` is linear.
std::vector<uint8_t> dataSet(uint8_t deviceId, uint32_t address, const uint8_t* data, size_t length) {
    const uint32_t packed = packAddress(address);
    std::vector<uint8_t> message = {0xF0, 0x41, deviceId, 0x16, 0x12, uint8_t(packed >> 16), uint8_t((packed >> 8) & 0x7F), uint8_t(packed & 0x7F)};
    message.insert(message.end(), data, data + length);
    unsigned int sum = 0;
    for (size_t i = 5; i < message.size(); i++) sum += message[i];
    message.push_back(uint8_t((128 - (sum & 0x7F)) & 0x7F));
    message.push_back(0xF7);
    return message;
}

struct RomEvent {
    uint32_t tick;
    uint8_t status;
    uint8_t data1;
    uint8_t data2;
};

// Decodes the event stream. Messages on channels no part listens to in ROM Play are dropped (a few stray notes
// on channel 16). Returns the tick of the end mark, with at most one long wait after the last message: Bumble Dee's
// data ends with six (18.8 s of silence after its last note-off), which the song should not have (the user); the
// other songs end within one (Folk's 4.2 s is the longest), so they are unchanged.
uint32_t decodeEvents(const std::vector<uint8_t>& data, std::vector<RomEvent>& events) {
    uint32_t ticks = 0;
    uint32_t trailingWaits = 0;  // Long waits since the last message
    uint8_t runningStatus = 0;
    for (size_t p = 0; p < data.size();) {
        const uint8_t delta = data[p++];
        if (delta == kLongWait) {
            ticks += 248;
            trailingWaits++;
            continue;
        }
        ticks += delta;
        if (p >= data.size() || data[p] == kEndMark) {
            if (trailingWaits > 1) ticks -= (trailingWaits - 1) * 248;
            break;
        }
        trailingWaits = 0;
        if (data[p] >= 0x80) runningStatus = data[p++];
        const uint8_t type = runningStatus & 0xF0;
        const size_t dataBytes = (type == 0xC0 || type == 0xD0) ? 1 : 2;
        if (p + dataBytes > data.size()) break;
        const RomEvent event{ticks, runningStatus, data[p], dataBytes > 1 ? data[p + 1] : uint8_t(0)};
        p += dataBytes;
        const unsigned int channel = runningStatus & 0x0F;
        if (channel >= kFirstPartChannel && channel <= kRhythmChannel) events.push_back(event);
    }
    return ticks;
}

// Builds the event list of one or more songs.
class SongWriter {
public:
    SongWriter(SmfFile& smf, uint8_t deviceId, bool forHardware) : smf_(smf), deviceId_(deviceId), forHardware_(forHardware) {}

    // Appends the setup for `song` from `start`; returns the time at which the setup is complete.
    double setup(const RomSong& song, const std::vector<RomEvent>& initialPrograms, double start) {
        time_ = start;
        spaced_ = forHardware_;
        // ROM Play resets the system area: 442 Hz, the song's reverb, partial reserves and parts on channels 2-10.
        std::vector<uint8_t> system = {kRomPlayMasterTune, song.reverb[0], song.reverb[1], song.reverb[2]};
        if (song.hasPartialReserve) {
            system.insert(system.end(), song.partialReserve, song.partialReserve + 9);
            for (unsigned int part = 0; part < 9; part++) system.push_back(uint8_t(kFirstPartChannel + part));
            sysex(dataSet(deviceId_, kSystemAddress, system.data(), system.size()));
        } else {
            sysex(dataSet(deviceId_, kSystemAddress, system.data(), system.size()));
            uint8_t channels[9];
            for (unsigned int part = 0; part < 9; part++) channels[part] = uint8_t(kFirstPartChannel + part);
            sysex(dataSet(deviceId_, kSystemAddress + 0x0D, channels, sizeof(channels)));
        }
        sysex(dataSet(deviceId_, kRhythmSetupAddress, song.rhythmSetup, sizeof(song.rhythmSetup)));
        // Part level 100, pan centre, full key range (the songs set volume and pan by controller).
        const uint8_t partDefaults[4] = {100, 7, 0, 127};
        for (uint32_t part = 0; part < 9; part++) sysex(dataSet(deviceId_, kPartTempAddress + part * 0x10 + 8, partDefaults, 4));
        for (unsigned int channel = kFirstPartChannel; channel <= kRhythmChannel; channel++) shortMessage(0xB0 | channel, 121, 0);
        for (const RomEvent& event : initialPrograms) program(song, event);
        spaced_ = false;
        return time_;
    }

    // Appends the song events with tick 0 at `origin`; returns the time of the end mark.
    double events(const RomSong& song, const std::vector<RomEvent>& events, uint32_t endTick, double origin) {
        // The songs send no note-offs to the rhythm part, whose tones ignore them anyway. Add them for other
        // sequencers and synths, a sixteenth note long or up to the next hit of the same key.
        const uint32_t noteLength = uint32_t(std::max(2.0, std::round(song.beatTicks > 0.0 ? song.beatTicks / 4.0 : 10.0)));
        std::vector<std::pair<uint32_t, uint8_t>> pendingOffs;  // (tick, key)
        auto flushOffs = [&](uint32_t upTo) {
            std::sort(pendingOffs.begin(), pendingOffs.end());
            size_t done = 0;
            for (; done < pendingOffs.size() && pendingOffs[done].first <= upTo; done++) {
                time_ = origin + pendingOffs[done].first * song.tickSeconds;
                shortMessage(0x90 | kRhythmChannel, pendingOffs[done].second, 0);
            }
            pendingOffs.erase(pendingOffs.begin(), pendingOffs.begin() + long(done));
        };
        for (const RomEvent& event : events) {
            flushOffs(event.tick);
            if (event.status == (0x90 | kRhythmChannel)) {
                // A note-on ends the previous hit of its key; a note-off from the song replaces the added one.
                for (auto& off : pendingOffs) {
                    if (off.second == event.data1) off.first = event.tick;
                }
                if (event.data2 == 0) {
                    pendingOffs.erase(std::remove_if(pendingOffs.begin(), pendingOffs.end(),
                                                     [&](const std::pair<uint32_t, uint8_t>& off) { return off.second == event.data1; }),
                                      pendingOffs.end());
                } else {
                    flushOffs(event.tick);
                    pendingOffs.emplace_back(event.tick + noteLength, event.data1);
                }
            }
            time_ = origin + event.tick * song.tickSeconds;
            if ((event.status & 0xF0) == 0xC0) {
                program(song, event);
            } else {
                shortMessage(event.status, event.data1, event.data2);
            }
        }
        for (auto& off : pendingOffs) off.first = std::min(off.first, endTick);
        flushOffs(endTick);
        return origin + endTick * song.tickSeconds;
    }

private:
    // A program change selects a timbre from the ROM Play bank; send the timbre (and a demo tone) after it.
    void program(const RomSong& song, const RomEvent& event) {
        shortMessage(event.status, event.data1, 0);
        const uint32_t part = (event.status & 0x0F) - kFirstPartChannel;
        if (part >= 8) return;  // The rhythm part ignores program changes
        const uint32_t timbreAddress = kPartTempAddress + part * 0x10;
        // d110emu reads byte 07H as the part's card and extra-bank flags, which the program change took from the
        // user's timbre memory; the unit ignores it, so exports leave it out.
        const size_t length = forHardware_ ? 7 : 8;
        if (!song.timbres) {
            // Without the firmware tables, fall back to the preset tone with the program's number.
            const uint8_t timbre[8] = {uint8_t(event.data1 / 64), uint8_t(event.data1 % 64), 24, 50, 12, 0, 1, 0};
            sysex(dataSet(deviceId_, timbreAddress, timbre, length));
            return;
        }
        const std::array<uint8_t, 8>& timbre = song.timbres->timbres[event.data1 & 63];
        if (timbre[0] == kDemoToneGroup) {
            // Demo tones are in no group a part can select: select the stand-in preset, then replace the part's
            // tone temporary area with the demo tone.
            const RomPlayTimbres::DemoTone& tone = song.timbres->tones[timbre[1]];
            if (tone.group <= 1) {
                const uint8_t standIn[8] = {tone.group, tone.number, timbre[2], timbre[3], timbre[4], timbre[5], timbre[6], 0};
                sysex(dataSet(deviceId_, timbreAddress, standIn, length));
            } else {
                const uint8_t rest[6] = {timbre[2], timbre[3], timbre[4], timbre[5], timbre[6], 0};
                sysex(dataSet(deviceId_, timbreAddress + 2, rest, length - 2));
            }
            sysex(dataSet(deviceId_, kToneTempAddress + part * kToneSize, tone.data.data(), tone.data.size()));
        } else {
            const uint8_t bytes[8] = {timbre[0], timbre[1], timbre[2], timbre[3], timbre[4], timbre[5], timbre[6], 0};
            sysex(dataSet(deviceId_, timbreAddress, bytes, length));
        }
    }

    void sysex(const std::vector<uint8_t>& message) {
        smf_.events.push_back(SmfEvent{time_, 0, uint32_t(smf_.sysexData.size()), uint32_t(message.size())});
        smf_.sysexData.insert(smf_.sysexData.end(), message.begin(), message.end());
        if (spaced_) time_ += double(message.size()) / kMidiBytesPerSecond + kHardwareSysexGap;
    }

    void shortMessage(uint8_t status, uint8_t data1, uint8_t data2) {
        smf_.events.push_back(SmfEvent{time_, uint32_t(status) | (uint32_t(data1) << 8) | (uint32_t(data2) << 16), 0, 0});
        if (spaced_) time_ += 3.0 / kMidiBytesPerSecond;
    }

    SmfFile& smf_;
    uint8_t deviceId_;
    bool forHardware_;
    bool spaced_ = false;  // Setup for real hardware: leave room after each message
    double time_ = 0.0;
};

// Splits off the program changes that come before the first note of their channel: they are sent with the
// setup instead, so that the song starts with its timbres in place.
void splitInitialPrograms(std::vector<RomEvent>& events, std::vector<RomEvent>& initialPrograms) {
    bool noteSeen[16] = {};
    std::vector<RomEvent> rest;
    for (const RomEvent& event : events) {
        const unsigned int channel = event.status & 0x0F;
        if ((event.status & 0xF0) == 0x90 && event.data2 != 0) noteSeen[channel] = true;
        if ((event.status & 0xF0) == 0xC0 && !noteSeen[channel]) {
            initialPrograms.push_back(event);
        } else {
            rest.push_back(event);
        }
    }
    events.swap(rest);
}

double quarterSeconds(const RomSong& song) {
    return (song.beatTicks > 0.0 ? song.beatTicks : 0.5 / song.tickSeconds) * song.tickSeconds;
}

}  // namespace

std::vector<RomSong> findRomSongs(const std::vector<uint8_t>& rom) {
    std::vector<RomSong> songs = readSongTable(rom);
    if (songs.empty()) songs = scanForSongs(rom);
    if (songs.empty()) return songs;

    const std::shared_ptr<const RomPlayTimbres> timbres = readTimbres(rom);
    const long reserveTable = findReserveTable(rom);
    for (size_t i = 0; i < songs.size(); i++) {
        RomSong& song = songs[i];
        song.timbres = timbres;
        if (reserveTable >= 0 && size_t(reserveTable) + 9 * (i + 1) <= rom.size()) {
            const uint8_t* reserve = &rom[size_t(reserveTable) + 9 * i];
            int total = 0;
            for (int part = 0; part < 9; part++) total += reserve[part];
            if (total <= 32) {
                std::memcpy(song.partialReserve, reserve, 9);
                song.hasPartialReserve = true;
            }
        }
        for (const SongTempo& tempo : kSongTempos) {
            if (song.name == tempo.name) {
                song.beatTicks = tempo.beatTicks;
                song.downbeatTick = tempo.downbeatTick;
            }
        }
    }
    return songs;
}

std::string findBootMessage(const std::vector<uint8_t>& rom) {
    static const char kMarker[] = "D-110  ver";
    const auto found = std::search(rom.begin(), rom.end(), kMarker, kMarker + sizeof(kMarker) - 1);
    if (found == rom.end() || found == rom.begin()) return std::string();
    const size_t start = size_t(found - rom.begin()) - 1;
    if (start + 32 > rom.size() || !std::all_of(rom.begin() + long(start), rom.begin() + long(start + 32), isNameChar)) return std::string();
    return std::string(reinterpret_cast<const char*>(&rom[start]), 32);
}

bool loadRomSongs(const std::filesystem::path& controlRomPath, std::vector<RomSong>& songs, std::string& error) {
    std::ifstream in(controlRomPath, std::ios::binary);
    if (!in) {
        error = "Cannot open " + controlRomPath.u8string();
        return false;
    }
    const std::vector<uint8_t> rom((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    songs = findRomSongs(rom);
    if (songs.empty()) {
        error = "This control ROM has no ROM Play songs";
        return false;
    }
    return true;
}

std::unique_ptr<SmfFile> romSongsToSmf(const std::vector<RomSong>& songs, uint8_t deviceId) {
    std::unique_ptr<SmfFile> smf(new SmfFile);
    SongWriter writer(*smf, deviceId, false);
    double time = 0.0;
    for (size_t i = 0; i < songs.size(); i++) {
        if (i > 0) time += kChainGapSeconds;
        std::vector<RomEvent> events;
        std::vector<RomEvent> initialPrograms;
        const uint32_t endTick = decodeEvents(songs[i].events, events);
        splitInitialPrograms(events, initialPrograms);
        const double setupEnd = writer.setup(songs[i], initialPrograms, time);
        time = writer.events(songs[i], events, endTick, setupEnd + kPlaybackLeadIn);
    }
    smf->duration = time;
    smf->title = songs.size() == 1 ? songs[0].name : std::string("Chain of Songs");
    // Keep events time-ordered for the player; equal times stay in insertion order.
    std::stable_sort(smf->events.begin(), smf->events.end(), [](const SmfEvent& a, const SmfEvent& b) { return a.time < b.time; });
    return smf;
}

bool saveRomSongSmf(const std::filesystem::path& path, const RomSong& song, uint8_t deviceId, std::string& error) {
    SmfFile smf;
    smf.title = song.name;
    SongWriter writer(smf, deviceId, true);
    std::vector<RomEvent> events;
    std::vector<RomEvent> initialPrograms;
    const uint32_t endTick = decodeEvents(song.events, events);
    splitInitialPrograms(events, initialPrograms);
    const double setupEnd = writer.setup(song, initialPrograms, 0.0);
    // Start the song on a bar line (4/4 at the song's tempo) once the setup is through.
    const double quarter = quarterSeconds(song);
    const double downbeat = song.beatTicks > 0.0 ? song.downbeatTick * song.tickSeconds : 0.0;
    int bars = 1;
    while (bars * 4 * quarter - downbeat < setupEnd + 0.1) bars++;
    smf.duration = writer.events(song, events, endTick, bars * 4 * quarter - downbeat);
    std::stable_sort(smf.events.begin(), smf.events.end(), [](const SmfEvent& a, const SmfEvent& b) { return a.time < b.time; });
    return saveSmfFile(path, smf, kExportTicksPerQuarter, uint32_t(std::lround(quarter * 1e6)), error);
}
